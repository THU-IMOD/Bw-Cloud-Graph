#!/bin/bash

# Script to sequentially run all experiment scripts in tools directory
# This script executes four experiment scripts in order:
# 1. run_throughput.sh - Throughput experiments
# 2. run_dyn_ratio.sh - Dynamic ratio experiments
# 3. run_overlap.sh - Overlap experiments
# 4. run_vtx_ops.sh - Vertex operations experiments

# Define the list of scripts to run
SCRIPTS=(
    "run_throughput.sh"
    "run_dyn_ratio.sh"
    "run_overlap.sh"
    "run_vtx_ops.sh"
)

# Tools directory
TOOLS_DIR="./tools"

# Color codes for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Print header
echo "=========================================="
echo "Sequential Experiment Runner"
echo "=========================================="
echo "Tools directory: $TOOLS_DIR"
echo "Scripts to run: ${SCRIPTS[@]}"
echo "Start time: $(date '+%Y-%m-%d %H:%M:%S')"
echo "=========================================="
echo ""

# Check if tools directory exists
if [ ! -d "$TOOLS_DIR" ]; then
    echo -e "${RED}✗ Error: Tools directory not found: $TOOLS_DIR${NC}"
    exit 1
fi

# Track overall start time
OVERALL_START=$(date +%s)

# Track success/failure
TOTAL_SCRIPTS=${#SCRIPTS[@]}
SUCCESSFUL=0
FAILED=0

# Loop through each script
for i in "${!SCRIPTS[@]}"; do
    script="${SCRIPTS[$i]}"
    script_path="$TOOLS_DIR/$script"
    script_num=$((i + 1))
    
    echo ""
    echo "=========================================="
    echo "[$script_num/$TOTAL_SCRIPTS] Running: $script"
    echo "=========================================="
    
    # Check if script exists
    if [ ! -f "$script_path" ]; then
        echo -e "${RED}✗ Error: Script not found: $script_path${NC}"
        echo "Skipping..."
        FAILED=$((FAILED + 1))
        continue
    fi
    
    # Record start time for this script
    SCRIPT_START=$(date +%s)
    echo "Script path: $script_path"
    echo "Start time: $(date '+%Y-%m-%d %H:%M:%S')"
    echo ""
    
    # Run the script with bash
    bash "$script_path"
    EXIT_CODE=$?
    
    # Record end time for this script
    SCRIPT_END=$(date +%s)
    SCRIPT_DURATION=$((SCRIPT_END - SCRIPT_START))
    
    echo ""
    echo "End time: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "Duration: ${SCRIPT_DURATION}s"
    
    # Check exit code
    if [ $EXIT_CODE -eq 0 ]; then
        echo -e "${GREEN}✓ Script completed successfully${NC}"
        SUCCESSFUL=$((SUCCESSFUL + 1))
    else
        echo -e "${RED}✗ Script failed with exit code $EXIT_CODE${NC}"
        FAILED=$((FAILED + 1))
        
        # Optional: Ask whether to continue
        echo ""
        read -p "Do you want to continue with remaining scripts? (y/n): " -n 1 -r
        echo ""
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            echo "Aborting remaining experiments..."
            break
        fi
    fi
    
    echo "=========================================="
    
    # Optional: Add a delay between scripts
    if [ $script_num -lt $TOTAL_SCRIPTS ]; then
        echo ""
        echo "Waiting 3 seconds before next script..."
        sleep 3
    fi
done

# Calculate overall duration
OVERALL_END=$(date +%s)
OVERALL_DURATION=$((OVERALL_END - OVERALL_START))

# Print summary
echo ""
echo ""
echo "=========================================="
echo "All Experiments Completed"
echo "=========================================="
echo "End time: $(date '+%Y-%m-%d %H:%M:%S')"
echo "Total duration: ${OVERALL_DURATION}s ($((OVERALL_DURATION / 60))m $((OVERALL_DURATION % 60))s)"
echo ""
echo "Summary:"
echo "  Total scripts: $TOTAL_SCRIPTS"
echo -e "  ${GREEN}Successful: $SUCCESSFUL${NC}"
if [ $FAILED -gt 0 ]; then
    echo -e "  ${RED}Failed: $FAILED${NC}"
else
    echo "  Failed: 0"
fi
echo "=========================================="
echo ""

# Exit with error code if any script failed
if [ $FAILED -gt 0 ]; then
    exit 1
else
    exit 0
fi