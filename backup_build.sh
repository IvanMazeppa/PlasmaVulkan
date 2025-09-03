#!/bin/bash
# Build Backup Script for PlasmaVulkan
# Usage: ./backup_build.sh [description]

# Get the next backup number
count=$(ls -d build_backups/*/ 2>/dev/null | wc -l)
count=$((count + 1))

# Pad the number with zeros
num=$(printf "%03d" $count)

# Get description from argument or use default
desc="${1:-backup}"

# Clean description (remove spaces and special chars)
desc=$(echo "$desc" | tr ' ' '_' | tr -cd '[:alnum:]_-')

# Create backup directory name
backup_dir="build_backups/${num}_${desc}"

# Create directory
echo "Creating backup: $backup_dir"
mkdir -p "$backup_dir"

# Copy the executable (try Debug first, then Release)
if [ -f "cmake-build-debug-visual-studio/Debug/PlasmaVulkan.exe" ]; then
    cp "cmake-build-debug-visual-studio/Debug/PlasmaVulkan.exe" "$backup_dir/"
elif [ -f "cmake-build-debug-visual-studio/Release/PlasmaVulkan.exe" ]; then
    cp "cmake-build-debug-visual-studio/Release/PlasmaVulkan.exe" "$backup_dir/"
fi

# Copy shader directory
if [ -d "cmake-build-debug-visual-studio/Debug/shaders" ]; then
    cp -r "cmake-build-debug-visual-studio/Debug/shaders" "$backup_dir/"
fi

# Create info file with timestamp and description
echo "Build Backup #$num" > "$backup_dir/info.txt"
echo "Date: $(date)" >> "$backup_dir/info.txt"
echo "Description: $desc" >> "$backup_dir/info.txt"

echo "Backup complete: $backup_dir"