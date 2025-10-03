#!/bin/bash
# Monitor calls in real-time

DB="whisper_talk.db"
LAST_ID=0

echo "🔍 Monitoring calls in database..."
echo "Waiting for new calls..."
echo ""

while true; do
    LATEST=$(sqlite3 "$DB" "SELECT id, call_id, status FROM calls ORDER BY id DESC LIMIT 1;" 2>/dev/null)
    if [ -n "$LATEST" ]; then
        CURRENT_ID=$(echo "$LATEST" | cut -d'|' -f1)
        if [ "$CURRENT_ID" != "$LAST_ID" ]; then
            echo "📞 NEW CALL: $LATEST"
            LAST_ID=$CURRENT_ID
        fi
    fi
    sleep 1
done

