#!/bin/sh

# Constants
DEVVM_NAME="getrafty.org"
DEVVM_BIND_DIR=$(pwd)
DEVVM_SSH_PORT=3333
DEVVM_WWW_PORT=3000

# Function to display error messages and exit
error_exit() {
    echo "Error: $1" >&2
    exit 1
}

# Function to check if Docker is installed
check_docker_installed() {
    command -v docker >/dev/null 2>&1 || error_exit "Docker is not installed. Please install Docker first."
}

# Function to install Docker based on the OS
install_docker() {
    case "$(uname)" in
    Linux)
        sudo apt-get update || error_exit "Failed to update package list."
        sudo apt-get install -y apt-transport-https ca-certificates curl gnupg lsb-release || error_exit "Failed to install prerequisites."
        curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /usr/share/keyrings/docker-archive-keyring.gpg || error_exit "Failed to add Docker's GPG key."
        echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/docker-archive-keyring.gpg] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list >/dev/null
        sudo apt-get update || error_exit "Failed to update package list."
        sudo apt-get install -y docker-ce docker-ce-cli containerd.io || error_exit "Failed to install Docker."
        sudo usermod -aG docker "$USER" || error_exit "Failed to add user to docker group."
        echo "Docker installed successfully. Please log out and log back in for changes to take effect."
        ;;
    Darwin)
        command -v brew >/dev/null 2>&1 || error_exit "Homebrew is not installed. Please install Homebrew first."
        brew install --cask docker || error_exit "Failed to install Docker using Homebrew."
        echo "Docker installed successfully. Please start Docker from the Applications folder."
        ;;
    *)
        error_exit "Unsupported OS type: $(uname)"
        ;;
    esac
}

# Function to check if the development VM is running and handle it accordingly
check_devvm_up() {
    container_id=$(docker ps --filter "name=$DEVVM_NAME" --format "{{.ID}}")
    if [ -n "$container_id" ]; then
        echo "DevVM $container_id is already running on port $DEVVM_SSH_PORT."
        echo "Would you like to shutdown it? (yes/no)"
        read answer
        [ "$answer" = "yes" ] && docker stop "$container_id" && docker rm "$container_id" && return 1
        return 0
    fi
    return 1
}

# ------------------------------------------------------------

# Commands

# Function to build and run the Docker container with optional deletion of old image
command_boot() {
    build=false

    while [ "$#" -gt 0 ]; do
        case "$1" in
        --build)
            build=true
            shift
            ;;
        *) break ;;
        esac
    done

    check_docker_installed

    if check_devvm_up; then
        echo "DevVM is up."
        exit 0
    fi

    if [ "$build" = true ]; then
        docker images "$DEVVM_NAME" -q | grep -q . && docker rmi "$DEVVM_NAME" || error_exit "Failed to delete old image."
        docker build -t "$DEVVM_NAME" . || error_exit "Failed to build Docker image."
    fi

    docker run -d \
        --name "$DEVVM_NAME" \
        --cap-add SYS_PTRACE \
        --cap-add SYS_ADMIN \
        --privileged \
        --device /dev/fuse:/dev/fuse \
        -v "$DEVVM_BIND_DIR:/home/me/workspace/" \
        -p "$DEVVM_SSH_PORT:22" \
        -p $DEVVM_WWW_PORT:$DEVVM_WWW_PORT \
        -e DEVVM_NAME="$DEVVM_NAME" \
        -e DEVVM_BIND_DIR="$DEVVM_BIND_DIR" \
        -e DEVVM_SSH_PORT="$DEVVM_SSH_PORT" \
        getrafty-wrapper || error_exit "Failed to start Docker container."

    echo "DevVM boot completed. Connect using SSH: ssh me@$(hostname -I | awk '{print $1}') -p $DEVVM_SSH_PORT, or simply: clippy attach"
}

# Function to attach to an already running DevVM
command_attach() {
    root=false

    while [ "$#" -gt 0 ]; do
        case "$1" in
        --root)
            root=true
            shift
            ;;
        *) break ;;
        esac
    done

    container_id=$(docker ps --filter "name=$DEVVM_NAME" --format "{{.ID}}")
    [ -z "$container_id" ] && error_exit "DevVM is not running."

    if [ "$root" = true ]; then
        docker exec -it "$container_id" /bin/bash
    else
        ssh me@$(hostname -I | awk '{print $1}') -p $DEVVM_SSH_PORT
    fi
}

# Determine the repository directory based on environment or location of script.
export CLIPPY_REPO="~/workspace"
if [ ! -e ~/.clippy ]; then
    script_dir=$(dirname "$(realpath "$0")")
    [ "$(pwd)" != "$script_dir" ] && error_exit "Please run clippy from git repository directory: $script_dir"
    export CLIPPY_REPO="$script_dir"
fi

# Parse command line arguments and execute the appropriate function or default command
case "$1" in
boot)
    shift
    command_boot "$@"
    ;;
attach)
    shift
    command_attach "$@"
    ;;
*) command_default ;;
esac
