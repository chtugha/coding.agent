#!/bin/bash

# Test chunked upload script
FILENAME="$1"
FILESIZE=$(stat -f%z "$FILENAME")
CHUNK_SIZE=1048576  # 1MB chunks
SERVER_URL="http://localhost:8081/api/whisper/upload"

echo "Testing chunked upload of $FILENAME ($FILESIZE bytes)"
echo "Chunk size: $CHUNK_SIZE bytes"

# Calculate number of chunks
TOTAL_CHUNKS=$(( (FILESIZE + CHUNK_SIZE - 1) / CHUNK_SIZE ))
echo "Total chunks: $TOTAL_CHUNKS"

# Upload each chunk
for ((i=0; i<TOTAL_CHUNKS; i++)); do
    START=$((i * CHUNK_SIZE))
    END=$((START + CHUNK_SIZE - 1))
    
    # Adjust end for last chunk
    if [ $END -ge $FILESIZE ]; then
        END=$((FILESIZE - 1))
    fi
    
    ACTUAL_CHUNK_SIZE=$((END - START + 1))
    
    echo "Uploading chunk $((i+1))/$TOTAL_CHUNKS: bytes $START-$END ($ACTUAL_CHUNK_SIZE bytes)"
    
    # Extract chunk
    dd if="$FILENAME" of="temp_chunk.bin" bs=1 skip=$START count=$ACTUAL_CHUNK_SIZE 2>/dev/null
    
    # Upload chunk
    RESPONSE=$(curl -s -X POST \
        -H "Content-Type: application/octet-stream" \
        -H "Content-Range: bytes $START-$END/$FILESIZE" \
        -H "X-File-Name: $(basename $FILENAME)" \
        -H "X-File-Size: $FILESIZE" \
        --data-binary @temp_chunk.bin \
        "$SERVER_URL")
    
    echo "Response: $RESPONSE"
    
    # Check if upload failed
    if echo "$RESPONSE" | grep -q "error"; then
        echo "❌ Upload failed!"
        rm -f temp_chunk.bin
        exit 1
    fi
    
    # Check if completed
    if echo "$RESPONSE" | grep -q '"success": true' && echo "$RESPONSE" | grep -q '"message": "Upload completed"'; then
        echo "✅ Upload completed successfully!"
        rm -f temp_chunk.bin
        exit 0
    fi
done

rm -f temp_chunk.bin
echo "✅ All chunks uploaded successfully!"
