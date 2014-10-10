// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include "watcher.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "util.h"


NativeWatcher::NativeWatcher() {
  // Use kqueue to implement file watching on OS X. kqueue does not support
  // watching directories, but using directories as inputs in ninja manifests
  // doesn't work well anyway because OSs only change directory mtimes if
  // direct children are touched.
  // The FSEvents API allows watching directory changes, but it doesn't easily
  // work with the pselect() call in subprocess-posix.cc.

  // kqueue fds aren't inherited by children.
  fd_ = kqueue();
}

NativeWatcher::~NativeWatcher() {
  close(fd_);
}

void NativeWatcher::AddPath(string path, void* key) {
  size_t pos = 0;
  subdir_map_type* map = &roots_;

  // Ensure we watch the current directory for relative paths.
  if (path[0] != '/')
    path = "./" + path;

  while (1) {
    size_t slash_offset = path.find('/', pos);
    string subdir = path.substr(pos, slash_offset - pos); // XXX: valid if npos?
    WatchedNode* subdir_node = &(*map)[subdir];

    if (slash_offset == string::npos) {
      subdir_node->key_ = key;
    }

    // XXX: if all leaf files exist, there's no need to watch directory nodes.
    if (!subdir_node->has_wd_ && slash_offset != 0) {
      string subpath = path.substr(0, slash_offset);
      // Closed when the event is processed:
      int wd = open(subpath.c_str(), O_CLOEXEC | O_EVTONLY);

      if (wd != -1) {
        pair<watch_map_type::iterator, bool> ins = watch_map_.insert(
            make_pair(wd, WatchMapEntry(subpath, subdir_node)));
        if (!ins.second) {
          // We are already watching this node through another path, e.g. via a
          // symlink. Rewrite path to use the existing path as a prefix.
          map->erase(subdir);
          if (slash_offset != string::npos) {
            path = ins.first->second.path_ + path.substr(slash_offset);
            slash_offset = ins.first->second.path_.size();
          }
          subdir_node = ins.first->second.node_;
        } else {
          subdir_node->it_ = ins.first;
          subdir_node->has_wd_ = true;

          struct kevent event;
          EV_SET(&event, wd, EVFILT_VNODE, EV_ADD | EV_CLEAR | EV_RECEIPT,
                 NOTE_DELETE | NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME |
                     NOTE_REVOKE | NOTE_EXTEND,
                 0, NULL);
          struct kevent response;
          int count = kevent(fd_, &event, 1, &response, 1, NULL);
          if (!count)
            Fatal("kevent: %s for %s", strerror(errno), subpath.c_str());
          if (response.flags & EV_ERROR && response.data)
            Fatal("kevent: %llx for %s", (unsigned long long)response.data,
                  subpath.c_str());
        }
      }
    }

    if (slash_offset == string::npos) {
      break;
    } else {
      pos = slash_offset + 1;
      map = &subdir_node->subdirs_;
    }
  }
}

void NativeWatcher::OnReady() {
  // Read only one event each time, to match the linux implementation.
  struct timespec timeout = { 0, 0 };
  struct kevent event;
  int count = kevent(fd_, NULL, 0, &event, 1, &timeout);

  if (!count)
    Fatal("kevent: %s", strerror(errno));
  if (event.flags & EV_ERROR)
    Fatal("kevent: %llx", (unsigned long long)event.data);

  WatchMapEntry* wme = &watch_map_[event.ident];
  if (!wme->node_) {
    // We've removed the watch, but we will continue to receive notifications
    // from before we removed it, which we can safely ignore.
    return;
  }

  if (event.fflags & NOTE_RENAME) {
    // The vnode was renamed to a different name that we may or may not care
    // about (we care only if it's a name we want to monitor but that didn't
    // exist yet). We definitely care about setting up a new watch at the
    // vnode's old path.
    // XXX: add test for caring about the old vnode.
    char buf[MAXPATHLEN];
    if(fcntl(event.ident, F_GETPATH, buf) == -1)
      Fatal("fcntl: %s", strerror(errno));
//fprintf(stderr, "  rename to %s\n", buf);
    //Refresh(buf, wme->node_);
    Refresh(wme->path_, wme->node_);
  }

  if (event.fflags & (NOTE_DELETE | NOTE_REVOKE | NOTE_ATTRIB)) {
    Refresh(wme->path_, wme->node_);
  }

  if (event.fflags & (NOTE_WRITE | NOTE_EXTEND)) {
    if (wme->node_->subdirs_.empty())  // File.
      result_.KeyChanged(wme->node_->key_);
    else {  // Directory.
      // NOTE_WRITE is sent for file creation (on directory vnodes). For all
      // subdirs that don't have a fd yet, check if one can be created now.
      for (subdir_map_type::iterator i = wme->node_->subdirs_.begin();
           i != wme->node_->subdirs_.end(); ++i) {
        if (!i->second.has_wd_) {
//fprintf(stderr, "  refreshing %s\n", i->first.c_str());
          Refresh(wme->path_ + "/" + i->first, &i->second);
        }
      }
    }
  }

  timeval tv;
  if (gettimeofday(&tv, NULL) < 0)  // XXX: query monotonic timer?
    Fatal("gettimeofday: %s", strerror(errno));
  TIMEVAL_TO_TIMESPEC(&tv, &last_refresh_);
}

void NativeWatcher::Refresh(const string& path, WatchedNode* node) {
  bool had_wd = node->has_wd_;
  if (had_wd) {
    close(node->it_->first);
    node->it_->second.node_ = 0;
    node->it_ = watch_map_type::iterator();
    node->has_wd_ = false;
  }

  // Closed when the event is processed, in the if above:
  int wd = open(path.c_str(), O_CLOEXEC | O_EVTONLY);
//fprintf(stderr, "got %d for %s\n", wd, path.c_str());
  if (wd != -1) {
    struct kevent event;
    EV_SET(&event, wd, EVFILT_VNODE, EV_ADD | EV_CLEAR | EV_RECEIPT,
           NOTE_DELETE | NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME | NOTE_REVOKE |
               NOTE_EXTEND,
           0, NULL);
    struct kevent response;
    int count = kevent(fd_, &event, 1, &response, 1, NULL);
    if (!count)
      Fatal("kevent: %s for %s", strerror(errno), path.c_str());
    if (response.flags & EV_ERROR && response.data)
      Fatal("kevent: %llx for %s", (unsigned long long)response.data,
            path.c_str());

    watch_map_[wd] = WatchMapEntry(path, node);
    node->it_ = watch_map_.find(wd);
    node->has_wd_ = true;
  }
  bool has_wd = node->has_wd_;

  if (node->key_) {
    if (had_wd && has_wd) {
      result_.KeyChanged(node->key_);
    } else if (had_wd && !has_wd) {
      result_.KeyDeleted(node->key_);
    } else if (!had_wd && has_wd) {
      result_.KeyAdded(node->key_);
    }
  }

  for (subdir_map_type::iterator i = node->subdirs_.begin();
       i != node->subdirs_.end(); ++i) {
    Refresh(path + "/" + i->first, &i->second);
  }
}

// XXX: this is near-identical to the linux impl
timespec* NativeWatcher::Timeout() {
  const long hysteresis_ns = 100000000;

  if (!result_.Pending())
    return 0;

  timeval tv;
  if (gettimeofday(&tv, NULL) < 0)  // XXX: query monotonic timer?
    Fatal("gettimeofday: %s", strerror(errno));

  timespec now;
  TIMEVAL_TO_TIMESPEC(&tv, &now);
  if (now.tv_sec > last_refresh_.tv_sec+1) {
    timeout_.tv_sec = 0;
    timeout_.tv_nsec = 0;
    return &timeout_;
  }

  long now_ns = now.tv_nsec;
  if (now.tv_sec != last_refresh_.tv_sec)
    now_ns += 1000000000;
  if (now_ns > last_refresh_.tv_nsec + hysteresis_ns) {
    timeout_.tv_sec = 0;
    timeout_.tv_nsec = 0;
    return &timeout_;
  }

  timeout_.tv_sec = 0;
  timeout_.tv_nsec = last_refresh_.tv_nsec + hysteresis_ns - now_ns;
  return &timeout_;
}

// Used by tests only, handled by the subprocess pselect in real life.
void NativeWatcher::WaitForEvents() {
  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    int ret = pselect(fd_ + 1, &fds, 0, 0, Timeout(), 0);

    switch (ret) {
    case 1:
      OnReady();
      break;

    case 0:
      return;

    case -1:
      Fatal("pselect: %s", strerror(errno));
    }
  }
}
