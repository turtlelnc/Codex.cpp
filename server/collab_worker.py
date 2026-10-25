#!/usr/bin/python3 -I
"""Unprivileged Git operations. The coordinator never runs Git as root."""
import json
import hashlib
import stat
import resource
import shutil
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

LIMIT = 64 * 1024 * 1024
ACTOR = os.environ.get('USER', 'codex')
TRUSTED = os.environ.get('CODEX_COLLAB_TRUSTED_GIT')
WORKTREE = os.environ.get('CODEX_COLLAB_TRUSTED_WORKTREE')
INITIALIZING = len(sys.argv)>1 and sys.argv[1]=='init'


def git(root, *args, check=True):
    context = ['-C', str(root)]
    if TRUSTED and WORKTREE and str(root)==WORKTREE and not INITIALIZING:
        context = ['--git-dir='+TRUSTED, '--work-tree='+WORKTREE, '-c', 'core.bare=false']
    command = ['/usr/bin/git', '-c', 'core.hooksPath=/dev/null', '-c', 'core.fsmonitor=false', '-c', 'user.name=' + ACTOR,
               '-c', 'user.email=' + ACTOR + '@codex.local', *context, *args]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and result.returncode:
        raise ValueError(result.stderr.decode(errors='replace')[-2000:])
    return result


def refresh_agent_metadata(root):
    # Never interpret .git/config supplied by the shared Agent. The trusted
    # repository is outside its sandbox. Recreate the Agent's convenience clone.
    if not TRUSTED or str(root)!=WORKTREE:
        return
    with tempfile.TemporaryDirectory(prefix='metadata-', dir=Path(TRUSTED).parent) as directory:
        clone = Path(directory) / 'clone'
        git(Path(directory), 'clone', '--no-checkout', '--no-hardlinks', '--branch', 'shared', TRUSTED, str(clone))
        git(clone, 'read-tree', 'HEAD')
        exclude(clone)
        old = root / '.git'
        if old.exists() or old.is_symlink():
            history=Path(TRUSTED).parent/'metadata-history';history.mkdir(mode=0o700,exist_ok=True)
            os.replace(old,history/uuid.uuid4().hex)
        os.replace(clone / '.git', old)


def output(value):
    print(json.dumps(value))


def exclude(root):
    path = root / '.git/info/exclude'
    with path.open('a') as stream:
        stream.write('\n/.codex_cpp/\n/.codex-cpp/\n')


def clean(root):
    if git(root, 'status', '--porcelain').stdout:
        raise ValueError('Working directory has uncommitted changes; commit or resolve them first')


def commit(root, message):
    git(root, 'add', '-A', '--', '.', ':!.codex_cpp', ':!.codex-cpp')
    if git(root, 'diff', '--cached', '--quiet', check=False).returncode:
        git(root, 'commit', '-m', message)
    return git(root, 'rev-parse', 'HEAD').stdout.decode().strip()


def bundle_input(directory):
    data = sys.stdin.buffer.read(LIMIT + 1)
    if len(data) > LIMIT:
        raise ValueError('Repository transfer exceeds 64 MiB')
    path = Path(directory) / 'transfer.bundle'
    path.write_bytes(data)
    return path


def merge(root, ref, message):
    clean(root)
    result = git(root, 'merge', '--no-ff', '-m', message, ref, check=False)
    if result.returncode:
        conflicts = git(root, 'diff', '--name-only', '--diff-filter=U').stdout.decode().splitlines()
        git(root, 'merge', '--abort')
        return {'merged': False, 'conflicts': conflicts, 'error': 'Merge stopped; conflict attempt was aborted'}
    return {'merged': True, 'head': git(root, 'rev-parse', 'HEAD').stdout.decode().strip()}


def safe_read(root, name, limit=1024*1024):
    parts=Path(name).parts
    if not parts or Path(name).is_absolute() or any(part in {'..','.git','.codex_cpp','.codex-cpp'} for part in parts):
        raise ValueError('Cannot read that workspace file')
    descriptor=os.open(root,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try:
        for part in parts[:-1]:
            child=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW,dir_fd=descriptor)
            os.close(descriptor);descriptor=child
        target=os.open(parts[-1],os.O_RDONLY|os.O_NOFOLLOW,dir_fd=descriptor)
        with os.fdopen(target,'rb') as stream:
            info=os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size>limit:
                raise ValueError('File exceeds review limit')
            content=stream.read(limit+1)
            if len(content)>limit:raise ValueError('File exceeds review limit')
            return content
    finally:
        os.close(descriptor)


def preview(root):
    tracked = git(root, 'diff', 'HEAD', '--no-ext-diff').stdout
    changes = git(root, 'status', '--porcelain').stdout
    digest = hashlib.sha256()
    for part in (changes,tracked):digest.update(len(part).to_bytes(8,'big')+part)
    size = len(tracked)
    untracked = git(root, 'ls-files', '--others', '--exclude-standard', '-z').stdout.split(b'\0')
    names = []
    for raw in sorted(filter(None, untracked)):
        name = os.fsdecode(raw)
        target = root / name
        names.append(name)
        info = target.lstat()
        if stat.S_ISLNK(info.st_mode):
            content = os.fsencode(os.readlink(target))
        elif stat.S_ISREG(info.st_mode) and info.st_size <= 16 * 1024 * 1024:
            content = safe_read(root,name,16*1024*1024)
        else:
            raise ValueError('Untracked file cannot be reviewed safely')
        size += len(content)
        if size > LIMIT:
            raise ValueError('Review exceeds repository transfer limit')
        digest.update(len(raw).to_bytes(8,'big')+raw+(info.st_mode&0o7777).to_bytes(8,'big')+len(content).to_bytes(8,'big')+content)
    return {'head': git(root, 'rev-parse', 'HEAD').stdout.decode().strip(),
            'fingerprint': digest.hexdigest(), 'diff': tracked[:524288].decode(errors='replace'),
            'untracked': names, 'truncated': len(tracked) > 524288}


def main():
    if os.getuid() == 0:
        raise ValueError('Git worker cannot run as root')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    for kind,maximum in ((resource.RLIMIT_NPROC,256),(resource.RLIMIT_NOFILE,4096)):
        _,hard=resource.getrlimit(kind);limit=maximum if hard==resource.RLIM_INFINITY else min(maximum,hard)
        resource.setrlimit(kind,(limit,limit))
    operation, *args = sys.argv[1:]
    if operation=='agent':
        root=Path(args[0]);os.chdir(root)
        os.execve('/usr/local/bin/codex-cpp', ['/usr/local/bin/codex-cpp','--root',str(root)]+args[1:], dict(os.environ))
    root = Path(args[0])
    with tempfile.TemporaryDirectory(prefix='codex-git-') as temp:
        if operation == 'init':
            root.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            bare = Path(args[1])
            source = args[2] if len(args) > 2 else ''
            if source:
                git(root.parent, 'clone', '--no-local', '--', source, str(root))
                git(root, 'checkout', '-B', 'shared')
                git(root, 'remote', 'remove', 'origin')
            else:
                root.mkdir(mode=0o700)
                git(root, 'init', '-b', 'shared')
                git(root, 'commit', '--allow-empty', '-m', 'Initialize collaborative project')
            git(root.parent, 'init', '--bare', '-b', 'shared', str(bare))
            git(root, 'remote', 'add', 'central', str(bare))
            git(root, 'push', 'central', 'HEAD:refs/heads/shared')
            exclude(root)
            (bare / 'info/exclude').write_text('/.codex_cpp/\n/.codex-cpp/\n')
            subprocess.run(['/usr/bin/git','--git-dir='+str(bare),'read-tree','HEAD'],check=True,stdout=subprocess.DEVNULL)
            output({'head': git(root, 'rev-parse', 'HEAD').stdout.decode().strip()})
        elif operation == 'export':
            path = Path(temp) / 'export.bundle'
            git(root, 'bundle', 'create', str(path), args[1])
            if path.stat().st_size > LIMIT:
                raise ValueError('Repository transfer exceeds 64 MiB')
            sys.stdout.buffer.write(path.read_bytes())
        elif operation == 'personal':
            if (root / '.git').exists():
                if (root / '.git').is_symlink() or git(root, 'rev-parse', '--show-toplevel').stdout.decode().strip() != str(root.resolve()):
                    raise ValueError('Personal workspace is not an independent clone')
                output({'workspace': str(root), 'head': git(root, 'rev-parse', 'HEAD').stdout.decode().strip()})
                return
            root.mkdir(mode=0o700, parents=True, exist_ok=True)
            if any(root.iterdir()):
                raise ValueError('Personal workspace must be empty for the initial clone')
            bundle = bundle_input(temp)
            git(root, 'init', '-b', args[1])
            git(root, 'fetch', str(bundle), 'HEAD')
            git(root, 'reset', '--hard', 'FETCH_HEAD')
            git(root,'config','user.name',ACTOR)
            git(root,'config','user.email',ACTOR+'@codex.local')
            exclude(root)
            output({'workspace': str(root), 'head': git(root, 'rev-parse', 'HEAD').stdout.decode().strip()})
        elif operation=='probe':
            import shutil
            probe=subprocess.run(['/usr/bin/bwrap','--unshare-all','--die-with-parent','--ro-bind','/','/','--proc','/proc','--dev','/dev','--tmpfs','/tmp','--','/usr/bin/true'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            output({'agent':os.access('/usr/local/bin/codex-cpp',os.X_OK),'git':bool(shutil.which('git')),'sandbox':probe.returncode==0})
        elif operation == 'diff':
            output(preview(root))
        elif operation == 'read':
            output({'path':args[1],'text':safe_read(root,args[1]).decode(errors='replace')})
        elif operation == 'refs':
            lines = git(root, 'for-each-ref', '--format=%(refname:short) %(objectname)', 'refs/heads/').stdout.decode().splitlines()
            output(dict(line.split(' ', 1) for line in lines))
        elif operation == 'status':
            output({'head': git(root, 'rev-parse', 'HEAD').stdout.decode().strip(),
                    'branch': git(root, 'branch', '--show-current').stdout.decode().strip(),
                    'changes': git(root, 'status', '--porcelain').stdout.decode().splitlines()})
        elif operation == 'commit':
            output({'head': commit(root, args[1])})
        elif operation == 'import':
            bundle = bundle_input(temp)
            ref = args[1]
            previous = git(root, 'rev-parse', '--verify', ref, check=False)
            git(root, 'fetch', str(bundle), 'HEAD')
            sha = git(root, 'rev-parse', 'FETCH_HEAD').stdout.decode().strip()
            old = previous.stdout.decode().strip() if previous.returncode == 0 else '0' * 40
            if old != '0' * 40 and git(root, 'merge-base', '--is-ancestor', old, sha, check=False).returncode:
                raise ValueError('Publish refused: personal branch history diverged; synchronize first')
            git(root, 'update-ref', ref, sha, old)
            output({'head': sha})
        elif operation == 'checkpoint':
            if preview(root)['fingerprint'] != args[2]:
                raise ValueError('Working files changed since confirmation; inspect again')
            sha = commit(root, args[1])
            refresh_agent_metadata(root)
            output({'head': sha})
        elif operation == 'merge':
            bare, ref, expected, target = args[1:]
            source = git(Path(bare), 'rev-parse', ref).stdout.decode().strip()
            actual = git(root, 'rev-parse', 'HEAD').stdout.decode().strip()
            if source != expected or actual != target:
                raise ValueError('Branches changed since confirmation; inspect current commits and confirm again')
            clean(root)
            git(root, 'fetch', bare, source)
            result = merge(root, 'FETCH_HEAD', f'Merge {ref} ({source[:12]}) approved by {ACTOR}')
            if result['merged']:
                refresh_agent_metadata(root)
            output(result)
        elif operation == 'sync':
            clean(root)
            bundle = bundle_input(temp)
            git(root, 'fetch', str(bundle), 'HEAD')
            output(merge(root, 'FETCH_HEAD', 'Synchronize shared project branch'))
        elif operation == 'personal-busy':
            socket = Path.home() / '.local/state/codex-server/tmux.sock'
            result = subprocess.run(['/usr/bin/tmux', '-S', str(socket), 'has-session', '-t', 'codex-team-' + args[0]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            output({'busy': result.returncode == 0})
        else:
            raise ValueError('Unknown worker operation')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        # Captured by the privileged coordinator; never returned to members.
        print("Git operation failed: "+str(error)[-2000:],file=sys.stderr)
        sys.exit(2)
