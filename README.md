
# Linux Backup Manager

An interactive backup manager for Linux, written in C. It mirrors a source directory to one or more backup directories, keeps the backups in sync in real time using inotify, and can restore a source directory from a backup.

Built as a university systems programming project to practise process management, signals, filesystem operations and the inotify API.

## Features

- **Multiple targets per source**: one `add` command can back up a directory to several locations at once.
- **One process per backup**: each source–target pair runs in its own child process created with `fork()`, so backups work in parallel and can be stopped independently.
- **Real-time mirroring**: after an initial full copy, changes are detected with inotify and applied to the backup immediately. Handled events: create, modify, attribute change, delete, move in and out, and deletion or move of a watched directory itself.
- **Incremental restore**: `restore` brings the source back to the state of the backup, skipping files whose size and modification time already match and removing files that are not in the backup.
- **Safety checks**: the source must be a directory, a target must be empty or not yet exist, and a target inside the source is rejected.
- **Clean shutdown**: `SIGINT` / `SIGTERM` and the `exit` command stop all child processes and reap them.

## Build

Requirements: Linux, a C17 compiler (gcc or clang) and `make`.

```bash
make
```

The default build enables AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer for development. A strict build without sanitizers that treats warnings as errors:

```bash
make CI=1
```

## Usage

```bash
./sop-backup
```

| Command | Description |
| --- | --- |
| `add <source> <target> [<target> ...]` | Copy the source to each target and keep it in sync |
| `end <source> <target> [<target> ...]` | Stop synchronising the given backups |
| `restore <source> <target>` | Restore the source directory from the backup in `<target>` |
| `list` | Show active backups with their process IDs |
| `exit` | Stop all backups and quit |

Paths containing spaces can be wrapped in double quotes.

```text
> add /home/user/docs /mnt/backup1 /mnt/backup2
Started backup: "/home/user/docs" -> "/mnt/backup1" (pid 1194)
Started backup: "/home/user/docs" -> "/mnt/backup2" (pid 1195)
> list
Active backups:
  "/home/user/docs" -> "/mnt/backup1" (pid 1194)
  "/home/user/docs" -> "/mnt/backup2" (pid 1195)
> end /home/user/docs /mnt/backup2
Stopped backup: "/home/user/docs" -> "/mnt/backup2"
```

## Project structure

| File | Responsibility |
| --- | --- |
| `sop-backup.c` | Command loop, input validation, process creation and signal handling |
| `parser.c` | Splits a command line into tokens, with support for quoted paths |
| `state.c` | Dynamic list of active backups and their child processes |
| `copy.c` | Initial recursive copy of the source to a target |
| `watch.c` | inotify watches on every subdirectory and mirroring of changes |
| `restore.c` | Restoring a source directory from a backup |

## Test results

Measured on a Linux machine with the default sanitizer build:

- 10,000 files (40 MB) copied to 3 targets in parallel in under 1 second; all three backups identical to the source.
- Single-file changes mirrored to the backup in about 2 ms on average over 20 changes.
- Creating, deleting, renaming and moving files and directories during a running backup were mirrored correctly, and `restore` returned a modified source to an exact copy of the backup.
- No errors reported by AddressSanitizer, UndefinedBehaviorSanitizer or LeakSanitizer during these runs.

## Known limitations

- Absolute symbolic links that point to a file inside the source directory are not yet rewritten correctly in the backup. Relative symbolic links are preserved as they are.
- Restore compares regular files by size and modification time only, not by content.
