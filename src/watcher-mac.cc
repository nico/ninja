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
#include <sys/time.h>
#include <unistd.h>

#include "util.h"

//#include <string.h>
//#include <sys/inotify.h>
//#include <sys/select.h>

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

void NativeWatcher::AddPath(std::string path, void* key) {
  size_t pos = 0;
  subdir_map_type* map = &roots_;

  // Ensure we watch the current directory for relative paths.
  if (path[0] != '/')
    path = "./" + path;

  while (1) {
    size_t slash_offset = path.find('/', pos);
    std::string subdir = path.substr(pos, slash_offset - pos);
    WatchedNode* subdir_node = &(*map)[subdir];

    if (slash_offset == std::string::npos) {
      subdir_node->key_ = key;
    }

    if (!subdir_node->has_wd_ && slash_offset != 0) {
      std::string subpath = path.substr(0, slash_offset);
      // Closed when the event is processed:
      int wd = open(subpath.c_str(), O_EVTONLY);

      if (wd != -1) {
        std::pair<watch_map_type::iterator, bool> ins = watch_map_.insert(
            std::make_pair(wd, WatchMapEntry(subpath, subdir_node)));
        if (!ins.second) {
          // We are already watching this node through another path, e.g. via a
          // symlink. Rewrite path to use the existing path as a prefix.
          map->erase(subdir);
          if (slash_offset != std::string::npos) {
            path = ins.first->second.path_ + path.substr(slash_offset);
            slash_offset = ins.first->second.path_.size();
          }
          subdir_node = ins.first->second.node_;
        } else {
          subdir_node->it_ = ins.first;
          subdir_node->has_wd_ = true;

          struct kevent event;
          EV_SET(&event, wd, EVFILT_VNODE, (EV_ADD | EV_CLEAR | EV_RECEIPT),
                 (NOTE_DELETE | NOTE_WRITE | NOTE_ATTRIB |
                  NOTE_RENAME | NOTE_REVOKE | NOTE_EXTEND), 0, NULL);
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

    if (slash_offset == std::string::npos) {
      break;
    } else {
      pos = slash_offset + 1;
      map = &subdir_node->subdirs_;
    }
  }
}

void NativeWatcher::OnReady() {
  // XXX: get events by calling kevent, do stuff
  // XXX: Refresh entries?

  timeval tv;
  if (gettimeofday(&tv, NULL) < 0)  // XXX: query monotonic timer?
    Fatal("gettimeofday: %s", strerror(errno));
  TIMEVAL_TO_TIMESPEC(&tv, &last_refresh_);
}

#if 0
void NativeWatcher::Refresh(const std::string& path, WatchedNode* node) {
  bool had_wd = node->has_wd_;
  if (had_wd) {
    inotify_rm_watch(fd_, node->it_->first);
    node->it_->second.node_ = 0;
    node->it_ = watch_map_type::iterator();
    node->has_wd_ = false;
  }

  int mask =
      IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF | IN_DELETE_SELF;
  if (node->key_) mask = IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF;

  int wd = inotify_add_watch(fd_, path.c_str(), mask);
  if (wd != -1) {
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
#endif

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
