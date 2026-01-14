#!/bin/bash
set -e

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Usage function
show_usage() {
    cat << EOF
Usage: sudo ./setup.sh [OPTIONS]

Setup and configure the SUDO Process Recorder plugin.

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

EOF
    exit 0
}

# Parse command line arguments
LOG_FILE=""
RUN_DAEMON=false
UNINSTALL=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_usage
            ;;
        -f|--log-file)
            if [[ -z "$2" || "$2" == -* ]]; then
                echo -e "${RED}Error: --log-file requires a file path argument${NC}"
                exit 1
            fi
            LOG_FILE="$2"
            shift 2
            ;;
        -d|--daemon)
            RUN_DAEMON=true
            shift
            ;;
        -u|--uninstall)
            UNINSTALL=true
            shift
            ;;
        *)
            echo -e "${RED}Error: Unknown option: $1${NC}"
            echo "Use -h or --help for usage information"
            exit 1
            ;;
    esac
done

# Disable the interceptor during setup to avoid interfering with command substitutions
unset LD_PRELOAD
unset SUDO_LOG_FILE

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Error: This script must be run as root${NC}"
    echo "Please run: sudo ./setup.sh"
    exit 1
fi

# Create log file if specified and not stdout
if [ -n "$LOG_FILE" ] && [ "$LOG_FILE" != "stdout" ]; then
    if [ ! -f "$LOG_FILE" ]; then
        touch "$LOG_FILE"
        chmod 644 "$LOG_FILE"
        echo -e "${GREEN}✓ Created log file: $LOG_FILE${NC}"
    fi
fi

# Handle uninstall
if [ "$UNINSTALL" = true ]; then
    echo "=== Uninstalling SUDO Process Recorder ==="
    
    # Stop daemon
    echo -e "${YELLOW}Stopping daemon processes...${NC}"
    pkill -f "sudo_daemon_collector" 2>/dev/null && echo -e "${GREEN}✓ Stopped daemon${NC}" || echo "No daemon running"
    rm -f /run/sudo-process-recorder.sock
    
    # Remove from sudo.conf
    echo -e "${YELLOW}Removing from /etc/sudo.conf...${NC}"
    if [ -f /etc/sudo.conf ]; then
        sed -i '/Plugin sudo_process_recorder/d' /etc/sudo.conf
        sed -i '/# SUDO Process Recorder Plugin/d' /etc/sudo.conf
        echo -e "${GREEN}✓ Removed from sudo.conf${NC}"
    fi
    
    # Remove files
    echo -e "${YELLOW}Removing installed files...${NC}"
    rm -f /usr/lib/sudo/sudo_process_recorder.so
    rm -f /usr/lib/syscall_interceptor.so
    echo -e "${GREEN}✓ Removed files${NC}"
    
    echo ""
    echo -e "${GREEN}=== Uninstall Complete ===${NC}"
    echo ""
    echo -e "${YELLOW}Note: To avoid LD_PRELOAD errors in current shell, run:${NC}"
    echo "  sudo ./setup.sh --uninstall && unset LD_PRELOAD && unset SUDO_LOG_FILE"
    echo ""
    exit 0
fi

echo "=== Building SUDO Process Recorder Setup ==="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
BUILD_OUT_LIB="$SCRIPT_DIR/build_out/lib"
BUILD_OUT_BIN="$SCRIPT_DIR/build_out/bin"

# Create build output directories
mkdir -p "$BUILD_OUT_LIB"
mkdir -p "$BUILD_OUT_BIN"

# 1. Compile the execve interceptor
echo -e "${YELLOW}[1/5] Compiling syscall_interceptor.c...${NC}"
gcc -shared -fPIC -o "$BUILD_OUT_LIB/syscall_interceptor.so" "$SRC_DIR/syscall_interceptor.c" -ldl
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Interceptor compiled successfully${NC}"
else
    echo -e "${RED}✗ Failed to compile interceptor${NC}"
    exit 1
fi

# 2. Compile the sudo process recorder plugin
echo -e "${YELLOW}[2/5] Compiling sudo_process_recorder.cpp...${NC}"
g++ -shared -fPIC -o "$BUILD_OUT_LIB/sudo_process_recorder.so" "$SRC_DIR/sudo_process_recorder.cpp"
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Plugin compiled successfully${NC}"
else
    echo -e "${RED}✗ Failed to compile plugin${NC}"
    exit 1
fi

# 3. Install the interceptor to /usr/lib/
echo -e "${YELLOW}[3/5] Installing syscall_interceptor.so to /usr/lib/...${NC}"
mkdir -p /usr/lib
install "$BUILD_OUT_LIB/syscall_interceptor.so" /usr/lib/
chmod 755 /usr/lib/syscall_interceptor.so
echo -e "${GREEN}✓ Interceptor installed at /usr/lib/syscall_interceptor.so${NC}"

# 4. Install the plugin to /usr/lib/sudo/
echo -e "${YELLOW}[4/5] Installing sudo_process_recorder.so to /usr/lib/sudo/...${NC}"
mkdir -p /usr/lib/sudo
install "$BUILD_OUT_LIB/sudo_process_recorder.so" /usr/lib/sudo/
chmod 755 /usr/lib/sudo/sudo_process_recorder.so
echo -e "${GREEN}✓ Plugin installed at /usr/lib/sudo/sudo_process_recorder.so${NC}"

# 5. Update sudo.conf if needed
SUDO_CONF="/etc/sudo.conf"
PLUGIN_LINE="Plugin sudo_process_recorder /usr/lib/sudo/sudo_process_recorder.so"

# Add log_file parameter if specified
if [ -n "$LOG_FILE" ]; then
    PLUGIN_LINE="$PLUGIN_LINE log_file=$LOG_FILE"
fi

echo -e "${YELLOW}[5/5] Configuring sudo.conf...${NC}"

# Check if sudo.conf exists
if [ ! -f "$SUDO_CONF" ]; then
    echo -e "${YELLOW}Creating $SUDO_CONF...${NC}"
    touch "$SUDO_CONF"
fi

# Remove old plugin configuration if exists
if grep -qF "Plugin sudo_process_recorder" "$SUDO_CONF"; then
    echo -e "${YELLOW}Updating existing plugin configuration...${NC}"
    sed -i '/Plugin sudo_process_recorder/d' "$SUDO_CONF"
    sed -i '/# SUDO Process Recorder Plugin/d' "$SUDO_CONF"
fi

# Add new plugin configuration
echo -e "${YELLOW}Adding plugin to $SUDO_CONF...${NC}"
echo "" >> "$SUDO_CONF"
echo "# SUDO Process Recorder Plugin" >> "$SUDO_CONF"
echo "$PLUGIN_LINE" >> "$SUDO_CONF"
echo -e "${GREEN}✓ Plugin added to $SUDO_CONF${NC}"

echo ""
echo -e "${GREEN}=== Setup Complete ===${NC}"
echo ""
echo "The sudo process recorder is now installed and configured."
echo "All sudo commands will now be tracked."
echo ""
echo "Configuration:"
echo "  - Interceptor: /usr/lib/syscall_interceptor.so"
echo "  - Plugin: /usr/lib/sudo/sudo_process_recorder.so"
echo "  - Config: $SUDO_CONF"
if [ -n "$LOG_FILE" ]; then
    echo "  - Log File: $LOG_FILE"
fi
echo ""

# 6. Start daemon if requested
if [ "$RUN_DAEMON" = true ]; then
    echo -e "${YELLOW}[6/6] Managing daemon collector...${NC}"
    
    # Define daemon paths
    DAEMON_SOCK="/run/sudo-process-recorder.sock"
    DAEMON_LOG="/var/log/sudo-process-recorder.jsonl"
    
    # Kill any existing daemons
    echo -e "${YELLOW}Stopping existing daemon processes...${NC}"
    EXISTING_PIDS=$(pgrep -f "sudo_daemon_collector" || true)
    if [ -n "$EXISTING_PIDS" ]; then
        echo "Found existing daemon(s) with PID(s): $EXISTING_PIDS"
        kill $EXISTING_PIDS 2>/dev/null || true
        sleep 1
        # Force kill if still running
        kill -9 $EXISTING_PIDS 2>/dev/null || true
        echo -e "${GREEN}✓ Stopped existing daemon(s)${NC}"
    else
        echo "No existing daemons found"
    fi
    
    # Check if daemon executable exists
    if [ ! -f "$BUILD_OUT_BIN/sudo_daemon_collector" ]; then
        echo -e "${YELLOW}Compiling sudo_daemon_collector...${NC}"
        g++ -o "$BUILD_OUT_BIN/sudo_daemon_collector" "$SRC_DIR/sudo_daemon_collector.cpp"
        if [ $? -ne 0 ]; then
            echo -e "${RED}✗ Failed to compile daemon${NC}"
            exit 1
        fi
    fi
    
    # Start daemon in background with proper arguments
    echo -e "${YELLOW}Starting new daemon...${NC}"
    nohup "$BUILD_OUT_BIN/sudo_daemon_collector" "$DAEMON_SOCK" "$DAEMON_LOG" > /var/log/sudo-daemon.log 2>&1 &
    DAEMON_PID=$!
    sleep 0.5  # Give daemon a moment to start
    
    # Verify daemon is running
    if kill -0 $DAEMON_PID 2>/dev/null; then
        echo -e "${GREEN}✓ Daemon started with PID $DAEMON_PID${NC}"
        echo "  - Daemon Debug Log: /var/log/sudo-daemon.log"
        echo "  - Process Records Audit Log: $DAEMON_LOG"
        echo "  - Socket: $DAEMON_SOCK"
    else
        echo -e "${RED}✗ Daemon failed to start. Check /var/log/sudo-daemon.log for errors${NC}"
    fi
    echo ""
fi

echo "To test, run: sudo ls"
echo ""
