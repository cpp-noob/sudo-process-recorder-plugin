# SUDO Process Recorder Plugin

A comprehensive Linux system monitoring tool that tracks and logs all processes spawned through `sudo` commands, including their child processes.

## Overview

The SUDO Process Recorder Plugin is a multi-component system that hooks into the sudo execution flow to capture detailed process information. It consists of:

- **Sudo I/O Plugin**: Integrates with sudo's plugin architecture to intercept command execution
- **Syscall Interceptor**: LD_PRELOAD library that captures child process creation (execve, execv)
- **Optional Daemon Collector**: Background service that aggregates process data into structured JSON logs

## Features

- ✅ **Zero Configuration Required**: Works out of the box with default stdout logging
- ✅ **Comprehensive Tracking**: Captures parent processes and all child processes spawned via execve, execv calls
- ✅ **Flexible Logging**: Output to stdout, file, or both
- ✅ **Daemon Mode**: Optional background collector for centralized logging

## How It Works

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  User executes: sudo <command>                              │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│  SUDO loads sudo_process_recorder.so plugin                 │
│  - Captures: user, command, PID, runas_user, etc.           │
│  - Sets LD_PRELOAD=syscall_interceptor.so                   │
│  - Sets SUDO_LOG_FILE environment variable                  │
│  - Logs to configured output (stdout/file)                  │
│  - Sends JSON to daemon socket (if daemon enabled)          │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│  Command executes with interceptor preloaded                │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│  Child process calls execve/execv            │
│  - Interceptor hooks the call                               │
│  - Logs: timestamp, PID, PPID, user, cwd, program, args     │
│  - Sends JSON to daemon socket (if daemon enabled)          │
│  - Logs to configured output (stdout/file)                  │
└─────────────────────────────────────────────────────────────┘
                         │
                         ▼ (if daemon enabled)
┌─────────────────────────────────────────────────────────────┐
│  sudo_daemon_collector (background process)                 │
│  - Receives JSON via Unix domain socket                     │
│  - Writes to /var/log/sudo-process-recorder.jsonl           │
└─────────────────────────────────────────────────────────────┘
```

### Build Process

The `setup.sh` script performs the following steps:

1. **Compilation**:
   - Compiles `syscall_interceptor.c` → `build_out/lib/syscall_interceptor.so`
   - Compiles `sudo_process_recorder.cpp` → `build_out/lib/sudo_process_recorder.so`
   - Compiles `sudo_daemon_collector.cpp` → `build_out/bin/sudo_daemon_collector` (if daemon mode)

2. **Installation**:
   - Creates necessary directories (`/usr/lib/sudo/`, etc.)
   - Installs `syscall_interceptor.so` to `/usr/lib/`
   - Installs `sudo_process_recorder.so` to `/usr/lib/sudo/`

3. **Configuration**:
   - Modifies `/etc/sudo.conf` to register the plugin
   - Adds optional `log_file` parameter if specified
   - Configures plugin to load on every sudo execution

4. **Daemon Setup** (optional):
   - Stops any existing daemon processes
   - Starts new daemon collector in background
   - Configures Unix domain socket at `/run/sudo-process-recorder.sock`

## Installation

### Prerequisites

- Linux operating system
- GCC/G++ compiler
- Root/sudo access
- sudo 1.8.0 or later (with plugin support)

### Build and Install

#### Option 1: Default Configuration (stdout logging, no daemon)

```bash
cd sudo-process-recorder-plugin/
sudo ./setup.sh
```

This configuration:
- Logs all process information to stdout (visible in terminal)
- No background daemon
- Minimal setup

#### Option 2: File Logging

```bash
sudo ./setup.sh -f /var/log/sudo-recorder.log
```

This configuration:
- Logs to specified file instead of stdout

#### Option 3: Daemon Mode

```bash
sudo ./setup.sh -d
```

This configuration:
- Starts background daemon collector
- Writes structured JSON to `/var/log/sudo-process-recorder.jsonl`

#### Option 4: File Logging + Daemon

```bash
sudo ./setup.sh -f /var/log/sudo-processes.log -d
```

This configuration:
- Combines file logging and daemon collection

### Command Line Options

```
Usage: sudo ./setup.sh [OPTIONS]

Options:
  -f, --log-file FILE    Log to FILE instead of stdout
                         (adds log_file parameter to plugin configuration)
  -d, --daemon           Run the daemon collector in the background
  -u, --uninstall        Uninstall the plugin and remove all components
  -h, --help             Display this help message and exit

Examples:
  sudo ./setup.sh                           # Install with stdout logging, no daemon
  sudo ./setup.sh -f /var/log/sudo.log      # Install with file logging
  sudo ./setup.sh -d                        # Install and start daemon
  sudo ./setup.sh -f /var/log/sudo.log -d   # Install with file logging and daemon
  sudo ./setup.sh --uninstall               # Uninstall the plugin
```

## Usage

Once installed, the plugin automatically activates for all sudo commands:

```bash
# Any sudo command will be tracked
sudo ls -la
sudo apt update
sudo systemctl restart nginx
sudo bash -c "echo test && ls"
```

### Example Output (stdout mode)

```
=== Basic Process Info ===
User: john | Command: /bin/bash | PID: 12345 | PPID: 12340
Tracking child processes via LD_PRELOAD...
==========================

[14:23:45] Sub-process 12346 (ppid=12345, user=0, cwd=/home/john): /bin/ls -la
[14:23:45] Sub-process 12347 (ppid=12345, user=0, cwd=/home/john): /usr/bin/grep test
```

### Example Output (daemon mode)

JSON log entries in `/var/log/sudo-process-recorder.jsonl`:

```json
{"basic_info":{"user":"john","pid":12345,"ppid":12340,"pgid":12340,"runas_user":"root","runas_uid":0,"runas_gid":0,"command":"/bin/bash"}}
{"type":"execve","pid":12346,"ppid":12345,"program":"/bin/ls","args":["/bin/ls","-la"]}
{"type":"execve","pid":12347,"ppid":12345,"program":"/usr/bin/grep","args":["/usr/bin/grep","test"]}
```

## Project Structure

```
sudo-process-recorder-plugin/
├── README.md
├── setup.sh                          # Installation script
├── .gitignore
├── build_out/                        # Build artifacts (generated)
│   ├── bin/                          # Compiled binaries
│   │   └── sudo_daemon_collector
│   └── lib/                          # Compiled libraries
│       ├── syscall_interceptor.so
│       └── sudo_process_recorder.so
└── src/
    ├── sudo_daemon_collector.cpp     # Background daemon for log collection
    ├── sudo_process_recorder.cpp     # Main sudo plugin
    └── syscall_interceptor.c         # LD_PRELOAD library for execve interception
```

## Uninstallation

To remove the plugin, simply run:

```bash
sudo ./setup.sh --uninstall
```

This will:
- Stop any running daemon processes
- Remove plugin configuration from `/etc/sudo.conf`
- Remove installed files from `/usr/lib/sudo/` and `/usr/lib/`

**Note**: If you see LD_PRELOAD errors after uninstalling, run the uninstall command with environment cleanup:

```bash
sudo ./setup.sh --uninstall && unset LD_PRELOAD && unset SUDO_LOG_FILE
```

Or simply start a new shell session.

## Troubleshooting

### Plugin not loading

Check `/etc/sudo.conf` contains:
```
Plugin sudo_process_recorder /usr/lib/sudo/sudo_process_recorder.so
```

### No child process logs

Verify LD_PRELOAD library exists:
```bash
ls -l /usr/lib/syscall_interceptor.so
```

### Daemon not starting

Check daemon logs:
```bash
cat /var/log/sudo-daemon.log
```

Verify socket exists:
```bash
ls -l /run/sudo-process-recorder.sock
```

## Security Considerations

- The plugin runs with elevated privileges (as part of sudo)
- Log files may contain sensitive command arguments
- Ensure proper file permissions on log outputs

## License

This project is provided as-is for educational and monitoring purposes.

## Contributing

Contributions are welcome! Please ensure:
- Code follows existing style
- Changes are tested on supported platforms
- Documentation is updated accordingly

## Technical Details

### Sudo Plugin API

The plugin implements the sudo I/O plugin interface (SUDO_IO_PLUGIN) with:
- `io_open`: Captures command execution context
- `io_close`: Cleanup and resource release

### Interceptor Implementation

Uses `dlsym(RTLD_NEXT, "execve")` to:
- Hook exec* family system calls
- Call original function after logging
- Preserve original program behavior

### Environment Variables

- `LD_PRELOAD`: Path to interceptor library
- `SUDO_DAEMON_SOCK`: Unix socket path for daemon communication
- `SUDO_LOG_FILE`: Log output destination ("stdout" or file path)
