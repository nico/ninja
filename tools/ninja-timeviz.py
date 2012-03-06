#!/usr/bin/env python

# XXX if useful, maybe this should become part of ninja? Maybe to c-specific?

import json
import os
import re
import subprocess

mostrecent = {}
data = []
for line in open('out/Release/.ninja_log'):
    if line.startswith('#'):
        continue
    start, end, restat, target, cmd = line.split('\t', 4)

    # Only keep compilations. Module shlex is a nicer way to do this, but
    # |shlex.split(cmd)| slows down the script more than 100x.

    if not 'clang' in cmd: continue
    dash_c = cmd.find(' -c ')
    if dash_c == -1: continue
    start_index = dash_c + len(' -c ')
    end_index = cmd.find(' ', start_index)
    source = os.path.normpath(
        os.path.join('out/Release', cmd[start_index:end_index]))

    data.append({
        'start': int(start),
        'end': int(end),
        'target': target,
        'source': source,
        'cmd': cmd.rstrip(),
        })
    mostrecent[target] = len(data) - 1

print 'name,t_ms,in_size_bytes,out_size_bytes,nlines'

# Keep only the most recent command for every file.
raw_data = data
data = []
n = 0.0
for i in range(len(raw_data)):
  d = raw_data[i]
  if i != mostrecent[d['target']]: continue

  p = n / (len(mostrecent) - 1)
  n += 1

  # XXX: lines of code (remove -MMD -MF foo.d -o foo.o, add -E -o tmp.ii, wc)
  cmd = d['cmd']
  ncmd = cmd.replace('-MMD ', '')
  ncmd = re.sub(r'-MF [^ ]+', '', ncmd)
  ncmd = re.sub(r'-o [^ ]+', '', ncmd)
  ncmd += ' -E -o tmp.ii'

  subprocess.check_call(ncmd, shell=True, cwd='out/Release')
  nlines = len(open('out/Release/tmp.ii').readlines())
  size = os.path.getsize('out/Release/tmp.ii')

  obj_size = os.path.getsize(os.path.join('out/Release', d['target']))

  print '%s,%d,%d,%d,%d' % (
      d['source'], d['end'] - d['start'], size, obj_size, nlines)

#  data.append({
#      't': d['end'] - d['start'],
#      'p': p,
#      'source': d['source'],
#      'nlines': n,
#      })
#
#
#out_format = 'json'
#
#if out_format == 'json':
#  print json.dumps({'data': data}, indent=2)
#elif out_format == 'csv':
#  keys = data[0].keys()
#  print ','.join(keys)
#  for d in data:
#    print ','.join([str(d[k]) for k in keys])
