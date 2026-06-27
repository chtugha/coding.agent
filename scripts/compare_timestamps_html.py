#!/usr/bin/env python3
"""
Compare timestamps from MFA alignment and Whisper.cpp output side by side in HTML
"""

import json
from pathlib import Path

def load_mfa_words(mfa_json_path):
    """Extract words and timestamps from MFA JSON output"""
    with open(mfa_json_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    words = []
    
    # Check if this is an aggregated output (new format)
    if 'words' in data and isinstance(data['words'], list):
        words = data['words']
    # Otherwise try old single-segment format
    elif 'tiers' in data:
        tiers = data['tiers']
        if isinstance(tiers, dict) and 'words' in tiers:
            entries = tiers['words'].get('entries', [])
            for entry in entries:
                start, end, text = entry
                if text:  # Skip empty entries
                    words.append({
                        'text': text,
                        'start': start,
                        'end': end,
                        'duration': end - start
                    })
    
    return words

def load_whisper_words(whisper_json_path):
    """Extract words and timestamps from Whisper.cpp merged JSON output"""
    with open(whisper_json_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    words = []
    
    # Check for merged format (new)
    if 'words' in data and isinstance(data['words'], list):
        for word_data in data['words']:
            words.append({
                'text': word_data['text'],
                'start': word_data['start'],
                'end': word_data['end'],
                'duration': word_data['end'] - word_data['start']
            })
    # Fallback to old transcription format
    elif 'transcription' in data:
        for segment in data['transcription']:
            if 'tokens' in segment:
                for token in segment['tokens']:
                    text = token['text'].strip()
                    # Skip special tokens and empty text
                    if text and not text.startswith('[_'):
                        start = token['offsets']['from'] / 1000.0  # Convert ms to seconds
                        end = token['offsets']['to'] / 1000.0
                        words.append({
                            'text': text,
                            'start': start,
                            'end': end,
                            'duration': end - start,
                            'confidence': token.get('p', 0.0)
                        })
    
    return words

def generate_html_comparison(mfa_words, whisper_words, output_path):
    """Generate HTML file comparing the two word lists"""
    
    # Generate MFA word entries
    mfa_html = ""
    for i, word in enumerate(mfa_words[:100]):  # Limit to first 100 words for readability
        mfa_html += f"""
        <div class="word-entry">
            <span class="word-text">{i+1}. {word['text']}</span><br>
            <span class="timestamp">[{word['start']:.3f}s - {word['end']:.3f}s]</span>
            <span class="duration">dur: {word['duration']:.3f}s</span>
        </div>"""
    
    # Generate Whisper word entries
    whisper_html = ""
    for i, word in enumerate(whisper_words[:100]):  # Limit to first 100 words
        confidence_html = ""
        if 'confidence' in word:
            confidence_html = f'<span class="confidence">p={word["confidence"]:.3f}</span>'
        
        whisper_html += f"""
        <div class="word-entry">
            <span class="word-text">{i+1}. {word['text']}</span><br>
            <span class="timestamp">[{word['start']:.3f}s - {word['end']:.3f}s]</span>
            <span class="duration">dur: {word['duration']:.3f}s</span>
            {confidence_html}
        </div>"""
    
    # Calculate durations
    mfa_duration = mfa_words[-1]['end'] if mfa_words else 0
    whisper_duration = whisper_words[-1]['end'] if whisper_words else 0
    
    # Build HTML with proper string formatting
    html = f"""<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Timestamp Comparison: MFA vs Whisper.cpp</title>
    <style>
        body {{
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            margin: 20px;
            background-color: #f5f5f5;
        }}
        .header {{
            background-color: #2c3e50;
            color: white;
            padding: 20px;
            border-radius: 8px;
            margin-bottom: 20px;
        }}
        .stats {{
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 15px;
            margin-bottom: 20px;
        }}
        .stat-card {{
            background: white;
            padding: 15px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        .stat-label {{
            font-size: 12px;
            color: #666;
            text-transform: uppercase;
        }}
        .stat-value {{
            font-size: 24px;
            font-weight: bold;
            color: #2c3e50;
        }}
        .comparison-container {{
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
            margin-bottom: 20px;
        }}
        .column {{
            background: white;
            border-radius: 8px;
            padding: 20px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        .column-header {{
            font-size: 18px;
            font-weight: bold;
            margin-bottom: 15px;
            padding-bottom: 10px;
            border-bottom: 2px solid #3498db;
            color: #2c3e50;
        }}
        .word-entry {{
            padding: 8px;
            margin: 4px 0;
            border-left: 3px solid #3498db;
            background-color: #f8f9fa;
            font-family: 'Courier New', monospace;
            font-size: 13px;
        }}
        .word-text {{
            font-weight: bold;
            color: #2c3e50;
        }}
        .timestamp {{
            color: #7f8c8d;
            font-size: 11px;
        }}
        .duration {{
            color: #27ae60;
            font-size: 11px;
        }}
        .confidence {{
            color: #e74c3c;
            font-size: 11px;
        }}
        .mfa-column .word-entry {{
            border-left-color: #9b59b6;
        }}
        .whisper-column .word-entry {{
            border-left-color: #3498db;
        }}
        .legend {{
            background: white;
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 20px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        .legend-item {{
            display: inline-block;
            margin-right: 20px;
            font-size: 12px;
        }}
        .legend-color {{
            display: inline-block;
            width: 20px;
            height: 3px;
            margin-right: 5px;
            vertical-align: middle;
        }}
    </style>
</head>
<body>
    <div class="header">
        <h1>📊 Timestamp Comparison: MFA vs Whisper.cpp</h1>
        <p>Episode 150 - First 5 Minutes</p>
    </div>
    
    <div class="stats">
        <div class="stat-card">
            <div class="stat-label">MFA Words</div>
            <div class="stat-value">{len(mfa_words)}</div>
        </div>
        <div class="stat-card">
            <div class="stat-label">Whisper Words</div>
            <div class="stat-value">{len(whisper_words)}</div>
        </div>
        <div class="stat-card">
            <div class="stat-label">MFA Duration</div>
            <div class="stat-value">{mfa_duration:.1f}s</div>
        </div>
        <div class="stat-card">
            <div class="stat-label">Whisper Duration</div>
            <div class="stat-value">{whisper_duration:.1f}s</div>
        </div>
    </div>
    
    <div class="legend">
        <div class="legend-item">
            <span class="legend-color" style="background-color: #9b59b6;"></span>
            MFA Alignment
        </div>
        <div class="legend-item">
            <span class="legend-color" style="background-color: #3498db;"></span>
            Whisper.cpp
        </div>
    </div>
    
    <div class="comparison-container">
        <div class="column mfa-column">
            <div class="column-header">🎯 MFA Alignment ({len(mfa_words)} words)</div>
            {mfa_html}
        </div>
        <div class="column whisper-column">
            <div class="column-header">🎤 Whisper.cpp ({len(whisper_words)} words)</div>
            {whisper_html}
        </div>
    </div>
</body>
</html>"""
    
    # Write HTML file
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(html)
    
    print(f"HTML comparison saved to: {output_path}")

def main():
    # Paths
    mfa_json = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test/episode_150_5min_mfa_aligned_full.json"
    whisper_json = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test/episode_150_5min_whisper_merged.json"
    output_html = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test/timestamp_comparison.html"
    
    print("Loading MFA alignment...")
    mfa_words = load_mfa_words(mfa_json)
    print(f"  Loaded {len(mfa_words)} words")
    
    print("Loading Whisper.cpp transcription...")
    whisper_words = load_whisper_words(whisper_json)
    print(f"  Loaded {len(whisper_words)} words")
    
    print("\nGenerating HTML comparison...")
    generate_html_comparison(mfa_words, whisper_words, output_html)
    
    print("\n✓ Done!")
    print(f"\nOpen in browser: file://{output_html}")

if __name__ == "__main__":
    main()

# Made with Bob