#!/usr/bin/python3
"""Manage one aibash tool workspace from outside the Bubblewrap sandbox."""

import difflib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import resource
import shutil
import stat
import subprocess
import sys


RUNTIME_ENV = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}
GIT_ENV = {**RUNTIME_ENV, "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null"}
PRIVATE_NAMES = {".git", ".ssh", ".gnupg", ".aws", ".bashllmrc", ".aibash_llm_history"}


def private_name(name):
    name = name.lower()
    return name in PRIVATE_NAMES or name == ".env" or name.startswith(".env.")


def exclude_private(directory, names):
    return [name for name in names if private_name(name)
            or not (Path(directory, name).is_symlink() or Path(directory, name).is_dir()
                    or Path(directory, name).is_file())]


def worker_identity():
    if os.getuid() != 0:
        return os.getuid(), os.getgid()
    try:
        account = pwd.getpwnam("magicbash-sandbox")
    except KeyError as error:
        raise RuntimeError("root sessions need a non-login magicbash-sandbox account") from error
    if not account.pw_uid or not account.pw_gid:
        raise RuntimeError("magicbash-sandbox must be an unprivileged account")
    return account.pw_uid, account.pw_gid


def init(stage, root, cwd, executable):
    if not os.access("/usr/bin/bwrap", os.X_OK):
        raise RuntimeError("Bubblewrap is required in /usr/bin/bwrap")
    uid, gid = worker_identity()
    root = Path(root)
    if stage.is_relative_to(root):
        raise RuntimeError("sandbox_root must not contain the staging directory")

    baseline = stage / "before"
    work = stage / "work"
    shutil.copytree(root, baseline, ignore=exclude_private, symlinks=True)
    baseline.chmod(0o700)
    shutil.copytree(baseline, work, symlinks=True)
    scratch = stage / "tmp"
    scratch.mkdir(mode=0o700)
    shutil.copyfile(executable, stage / "worker")
    (stage / "worker").chmod(0o755)
    if os.getuid() == 0:
        for directory, dirs, files in os.walk(work):
            for name in dirs + files:
                os.chown(Path(directory, name), uid, gid, follow_symlinks=False)
        os.chown(work, uid, gid)
        os.chown(scratch, uid, gid)
    (stage / "root").write_text(str(root))
    (stage / "cwd").write_text(cwd)
    stage.chmod(0o711)


def tool(stage, name, args, mode):
    uid, gid = worker_identity()
    command = [
        "/usr/bin/bwrap", "--unshare-user", "--unshare-pid", "--unshare-net",
        "--unshare-ipc", "--unshare-uts", "--new-session", "--die-with-parent",
        "--clearenv", "--setenv", "PATH", "/usr/bin:/bin",
        "--setenv", "HOME", "/tmp", "--setenv", "LANG", "C.UTF-8",
        "--ro-bind", "/usr", "/usr", "--ro-bind", "/bin", "/bin",
        "--ro-bind-try", "/lib", "/lib", "--ro-bind-try", "/lib64", "/lib64",
        "--proc", "/proc", "--dev", "/dev", "--bind", stage / "tmp", "/tmp",
        "--ro-bind", stage / "worker", "/aibash-worker",
        "--bind", stage / "work", "/work",
        "--chdir", (stage / "cwd").read_text(),
        "--", "/aibash-worker", "--aibash-sandbox-worker", name, args, mode,
    ]

    def drop_privileges():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        if os.getuid() == 0:
            os.setgroups([])
            os.setgid(gid)
            os.setuid(uid)

    result = subprocess.run(
        command, cwd="/", env=RUNTIME_ENV, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE if name == "cd" else None,
        preexec_fn=drop_privileges, timeout=120,
    )
    if name == "cd" and result.returncode == 0 and result.stdout:
        destination = result.stdout.decode(encoding="utf-8", errors="replace")
        if destination.startswith("/"):
            (stage / "cwd").write_text(destination)
        sys.stdout.buffer.write(result.stdout)
    return result.returncode if result.returncode >= 0 else 128 - result.returncode


def entry(path):
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return None
    if stat.S_ISDIR(mode):
        return ("directory",)
    if stat.S_ISLNK(mode):
        return ("symlink", os.readlink(path))
    if stat.S_ISREG(mode):
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(131072), b""):
                digest.update(block)
        return ("file", bool(mode & 0o111), digest.hexdigest())
    return ("special",)


def inventory(root):
    items = {}
    for directory, dirs, files in os.walk(root):
        for name in dirs + files:
            path = Path(directory, name)
            items[str(path.relative_to(root))] = entry(path)
    return items


def changes(stage):
    before = inventory(stage / "before")
    after = inventory(stage / "work")
    return [(name, before.get(name), after.get(name))
            for name in sorted(before.keys() | after.keys()) if before.get(name) != after.get(name)]


def destination(root, name):
    path = root
    for component in Path(name).parts:
        if component in (".", "..") or private_name(component):
            raise RuntimeError(f"protected project path {name!r}")
        path /= component
        if path.is_symlink():
            raise RuntimeError(f"symbolic link in project path {name!r}")
    return path


def verify(root, proposed):
    old_paths = {name for name, old, _ in proposed if old}
    for name, old, new in proposed:
        target = destination(root, name)
        kinds = {value[0] for value in (old, new) if value}
        if len(kinds) != 1 or not kinds <= {"file", "directory"}:
            raise RuntimeError(f"only ordinary files and directories can be applied: {name!r}")
        if entry(target) != old:
            raise RuntimeError(f"project changed since the sandbox started: {name!r}")
        if old == ("directory",) and new is None:
            if any(str(child.relative_to(root)) not in old_paths for child in target.iterdir()):
                raise RuntimeError(f"directory has files outside this change set: {name!r}")


def git(*args, cwd):
    return subprocess.run(["/usr/bin/git", *args], cwd=cwd, env=GIT_ENV,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)


def make_patch(stage):
    result = git("diff", "--no-index", "--binary", "--no-ext-diff", "--no-textconv",
                 "--no-renames", "--", "before", "work", cwd=stage)
    if result.returncode not in (0, 1):
        raise RuntimeError(result.stderr.decode(errors="replace").strip())
    return result.stdout


def display(text):
    return "".join(c if c.isprintable() or c == "\t" else f"\\x{ord(c):02x}" for c in text)


def preview(stage):
    proposed = changes(stage)
    if not proposed:
        return
    root = Path((stage / "root").read_text())
    verify(root, proposed)
    (stage / "patch").write_bytes(make_patch(stage))
    (stage / "proposal").write_text(json.dumps(proposed))

    print("Sandbox changes")
    for name, old, new in proposed[:200]:
        marker = "+" if old is None else "-" if new is None else "~"
        print(marker, repr(name))
    if len(proposed) > 200:
        print(f"... {len(proposed) - 200} more paths")

    diff_lines = 0
    for name, old, new in proposed:
        if (new or old)[0] != "file":
            continue
        base_path, new_path = stage / "before" / name, stage / "work" / name
        if any(path.stat().st_size > 16384 for path, state in ((base_path, old), (new_path, new)) if state):
            continue
        try:
            before = base_path.read_text(encoding="utf-8").splitlines() if old else []
            after = new_path.read_text(encoding="utf-8").splitlines() if new else []
        except UnicodeError:
            continue
        for line in difflib.unified_diff(before, after, repr(name), repr(name), lineterm=""):
            print(display(line[:400]))
            diff_lines += 1
            if diff_lines == 120:
                print("... diff preview truncated")
                return


def apply(stage):
    proposed = changes(stage)
    if json.dumps(proposed) != (stage / "proposal").read_text():
        raise RuntimeError("sandbox changed after the preview")
    root = Path((stage / "root").read_text())
    patch = stage / "patch"
    patch_command = ("apply", "-p2", "--whitespace=nowarn")
    lock = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX)
        verify(root, proposed)
        if patch.stat().st_size:
            result = git(*patch_command, "--check", str(patch), cwd=root)
            if result.returncode:
                raise RuntimeError(result.stderr.decode(errors="replace").strip())

        created = []
        try:
            for name, old, new in proposed:
                if old is None and new == ("directory",):
                    path = destination(root, name)
                    path.mkdir()
                    created.append(path)
            if patch.stat().st_size:
                result = git(*patch_command, str(patch), cwd=root)
                if result.returncode:
                    raise RuntimeError(result.stderr.decode(errors="replace").strip())
        except (OSError, RuntimeError):
            for path in reversed(created):
                if path.is_dir() and not any(path.iterdir()):
                    path.rmdir()
            raise

        for name, old, new in reversed(proposed):
            if old == ("directory",) and new is None:
                path = destination(root, name)
                if path.exists():
                    path.rmdir()
    finally:
        os.close(lock)
    print("aibash: sandbox changes applied", file=sys.stderr)


def main():
    action, location, *arguments = sys.argv[1:]
    stage = Path(location)
    if stage.parent != Path("/tmp") or not stage.name.startswith("aibash-sandbox.") or stage.is_symlink():
        raise RuntimeError("invalid sandbox staging directory")
    if action == "init":
        init(stage, *arguments)
    elif action == "tool":
        return tool(stage, *arguments)
    elif action == "preview":
        preview(stage)
    elif action == "apply":
        apply(stage)
    elif action == "clean":
        shutil.rmtree(stage)
    else:
        raise RuntimeError("unknown sandbox helper action")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        print(f"aibash: sandbox: {error}", file=sys.stderr)
        sys.exit(1)
