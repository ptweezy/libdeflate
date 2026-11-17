#!/usr/bin/env python3
"""r8_time sysinfo.py: describe the machine a shard runs on (test-only).

usage: sysinfo.py cpus               prints "TIMING_CPU DRIVER_CPUS": the
                                     last CPU for the timed processes, and
                                     the CPUs outside its core (all other
                                     CPUs if there is no other core) for
                                     everything else
       sysinfo.py meta OUT.json K=V...   writes the shard's meta.json: the
                                     K=V pairs plus CPU model, kernel,
                                     compiler and linker versions, ...
"""
import json, os, platform, re, subprocess, sys, time


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30).stdout.strip()
    except Exception as e:  # noqa
        return 'ERROR %s' % e


def read(path):
    try:
        return open(path).read().strip()
    except OSError:
        return ''


def topology(every=False):
    """[(cpu, (socket, core))] of the CPUs this process may use, or of every
    online CPU."""
    allowed = sorted(os.sched_getaffinity(0))
    cores = {}
    for line in sh('lscpu -p=CPU,CORE,SOCKET').splitlines():
        if line.startswith('#') or not line.strip():
            continue
        p = line.split(',')
        try:
            cores[int(p[0])] = (p[2], p[1])
        except (ValueError, IndexError):
            pass
    return [(c, cores.get(c, ('?', c))) for c in (sorted(cores) if every and cores else allowed)]


def cpus():
    topo = topology()
    tcpu, tcore = topo[-1]
    others = [c for c, k in topo if k != tcore]
    if not others:
        others = [c for c, k in topo if c != tcpu] or [tcpu]
    return tcpu, others


def cpu_model():
    ci = read('/proc/cpuinfo')
    m = re.search(r'^model name\s*:\s*(.*)$', ci, re.M)
    if m:
        return re.sub(r'\s+', ' ', m.group(1)).strip()
    ls = dict((k.strip(), v.strip()) for k, _, v in
              (line.partition(':') for line in sh('lscpu').splitlines()))
    impl = re.search(r'^CPU implementer\s*:\s*(\S+)', ci, re.M)
    part = re.search(r'^CPU part\s*:\s*(\S+)', ci, re.M)
    return '%s (%s, impl %s part %s)' % (ls.get('Model name', '?'), ls.get('Vendor ID', '?'),
                                         impl.group(1) if impl else '?', part.group(1) if part else '?')


if len(sys.argv) >= 2 and sys.argv[1] == 'cpus':
    t, o = cpus()
    print('%d %s' % (t, ','.join(str(c) for c in o)))
elif len(sys.argv) >= 3 and sys.argv[1] == 'meta':
    meta = dict(kv.split('=', 1) for kv in sys.argv[3:])
    ci = read('/proc/cpuinfo')
    meta.update({
        'arch': platform.machine(),
        'cpu_model': cpu_model(),
        'cpuinfo_first': ci.split('\n\n')[0],
        'lscpu': sh('lscpu'),
        'topology': [[c, list(k)] for c, k in topology(True)],
        'ncpu': os.cpu_count(),
        'mem_kb': (re.search(r'MemTotal:\s*(\d+)', read('/proc/meminfo')) or [0, '0'])[1],
        'kernel': platform.release(),
        'kernel_version': platform.version(),
        'os_release': sh('. /etc/os-release && echo "$PRETTY_NAME"'),
        'image': '%s %s' % (os.environ.get('ImageOS', ''), os.environ.get('ImageVersion', '')),
        'gcc': sh('gcc --version | head -1'),
        'clang': sh('clang --version | head -1'),
        'ld': sh('ld --version | head -1'),
        'python': platform.python_version(),
        'thp': read('/sys/kernel/mm/transparent_hugepage/enabled'),
        'perf_event_paranoid': read('/proc/sys/kernel/perf_event_paranoid'),
        'hypervisor': sh('systemd-detect-virt 2>/dev/null'),
        'loadavg': read('/proc/loadavg'),
        'time': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'runner_name': os.environ.get('RUNNER_NAME', ''),
        'runner_os': os.environ.get('RUNNER_OS', ''),
        'run_id': os.environ.get('GITHUB_RUN_ID', ''),
        'run_attempt': os.environ.get('GITHUB_RUN_ATTEMPT', ''),
        'ref': os.environ.get('GITHUB_REF_NAME', ''),
        'repo': os.environ.get('GITHUB_REPOSITORY', ''),
    })
    json.dump(meta, open(sys.argv[2], 'w'), indent=1)
    print('meta: %s | %s | kernel %s | %s | %s | %s' % (
        meta['cpu_model'], meta['arch'], meta['kernel'], meta['gcc'], meta['clang'], meta['ld']))
else:
    sys.exit(__doc__)
