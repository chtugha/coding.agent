#!/bin/bash
# Monitor WhisperX validation progress every 2 minutes

echo "========================================================================"
echo "Monitoring WhisperX Validation Progress"
echo "========================================================================"
echo ""
echo "Started at: $(date)"
echo "Checking every 2 minutes..."
echo ""

while true; do
    echo "----------------------------------------"
    echo "Check at: $(date '+%H:%M:%S')"
    echo "----------------------------------------"
    
    # Check if process is still running
    if ps aux | grep -v grep | grep "validate_mfa_cleaned_ep151.py" > /dev/null; then
        # Get process info
        ps aux | grep -v grep | grep "validate_mfa_cleaned_ep151.py" | awk '{printf "CPU: %s%% | Memory: %s%% | Time: %s\n", $3, $4, $10}'
        
        # Check for output file
        if [ -f "/tmp/fixed_alignment_test/episode_151_whisperx_validation.json" ]; then
            echo "✓ WhisperX output file created!"
            ls -lh /tmp/fixed_alignment_test/episode_151_whisperx_validation.json
            echo ""
            echo "Validation likely complete. Check terminal output."
            break
        else
            echo "Status: Still processing (no output file yet)"
        fi
    else
        echo "✓ Process completed!"
        echo ""
        
        # Check for output file
        if [ -f "/tmp/fixed_alignment_test/episode_151_whisperx_validation.json" ]; then
            echo "✓ WhisperX output file found:"
            ls -lh /tmp/fixed_alignment_test/episode_151_whisperx_validation.json
        else
            echo "⚠ No output file found - check for errors"
        fi
        
        echo ""
        echo "Check the terminal for validation results."
        break
    fi
    
    echo ""
    
    # Wait 2 minutes
    sleep 120
done

echo ""
echo "========================================================================"
echo "Monitoring Complete"
echo "========================================================================"

# Made with Bob
