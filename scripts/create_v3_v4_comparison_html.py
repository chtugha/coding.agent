#!/usr/bin/env python3
"""Create HTML visualization comparing V3 and V4 alignments"""

import json

# Load transcript
transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts/episode_153_gedanken_zum_ausdrucken_bringen.json"
with open(transcript_path, 'r') as f:
    data = json.load(f)
segments = data['segments']

# V3 and V4 offsets
v3_offset = 46.0
v4_offset = 14.0
offset_diff = v3_offset - v4_offset

# Select segments to display (first 50, around minute 24, around minute 48)
display_segments = []

# First 50 segments
display_segments.extend(segments[:50])

# Around minute 24 (1440s)
for seg in segments:
    if 1430 <= seg['start'] <= 1450:
        display_segments.append(seg)

# Around minute 48 (2880s)
for seg in segments:
    if 2870 <= seg['start'] <= 2890:
        display_segments.append(seg)

# Remove duplicates and sort
display_segments = sorted(list({seg['start']: seg for seg in display_segments}.values()), key=lambda x: x['start'])

# Create HTML
html = """<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>V3 vs V4 Alignment Comparison - Episode 153</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            margin: 20px;
            background-color: #f5f5f5;
        }
        .header {
            background-color: #2c3e50;
            color: white;
            padding: 20px;
            border-radius: 8px;
            margin-bottom: 20px;
        }
        .header h1 {
            margin: 0 0 10px 0;
        }
        .stats {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 15px;
            margin-bottom: 20px;
        }
        .stat-box {
            background: white;
            padding: 15px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }
        .stat-label {
            font-size: 12px;
            color: #666;
            text-transform: uppercase;
            margin-bottom: 5px;
        }
        .stat-value {
            font-size: 24px;
            font-weight: bold;
            color: #2c3e50;
        }
        .comparison-table {
            background: white;
            border-radius: 8px;
            overflow: hidden;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }
        table {
            width: 100%;
            border-collapse: collapse;
        }
        th {
            background-color: #34495e;
            color: white;
            padding: 12px;
            text-align: left;
            font-weight: 600;
        }
        td {
            padding: 10px 12px;
            border-bottom: 1px solid #ecf0f1;
        }
        tr:hover {
            background-color: #f8f9fa;
        }
        .transcript-time {
            font-weight: bold;
            color: #2980b9;
        }
        .v3-time {
            color: #27ae60;
        }
        .v4-time {
            color: #e74c3c;
        }
        .text-cell {
            max-width: 400px;
        }
        .section-header {
            background-color: #ecf0f1;
            font-weight: bold;
            text-align: center;
            padding: 15px;
        }
        .warning {
            background-color: #fff3cd;
            border: 2px solid #ffc107;
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 20px;
        }
        .warning-title {
            font-weight: bold;
            color: #856404;
            margin-bottom: 10px;
        }
    </style>
</head>
<body>
    <div class="header">
        <h1>🔍 V3 vs V4 Alignment Comparison</h1>
        <p>Episode 153: "GEDANKEN ZUM AUSDRUCKEN BRINGEN"</p>
        <p>Audio Duration: 5,569.3 seconds (92.8 minutes)</p>
    </div>

    <div class="warning">
        <div class="warning-title">⚠️ CRITICAL ISSUE DETECTED</div>
        <p><strong>V3 and V4 disagree by 32 seconds on where speech begins!</strong></p>
        <p>This means one algorithm is misaligning the entire episode. All timestamps will be off by 32 seconds.</p>
        <ul>
            <li>V3 detected speech starting at <strong>46.0 seconds</strong> in audio</li>
            <li>V4 detected speech starting at <strong>14.0 seconds</strong> in audio</li>
            <li>Difference: <strong>32.0 seconds</strong></li>
        </ul>
    </div>

    <div class="stats">
        <div class="stat-box">
            <div class="stat-label">V3 Offset</div>
            <div class="stat-value" style="color: #27ae60;">46.0s</div>
        </div>
        <div class="stat-box">
            <div class="stat-label">V4 Offset</div>
            <div class="stat-value" style="color: #e74c3c;">14.0s</div>
        </div>
        <div class="stat-box">
            <div class="stat-label">Disagreement</div>
            <div class="stat-value" style="color: #e67e22;">32.0s</div>
        </div>
    </div>

    <div class="comparison-table">
        <table>
            <thead>
                <tr>
                    <th>Transcript Time</th>
                    <th>V3 Audio Time<br><span style="font-weight:normal;font-size:11px;">(+46s offset)</span></th>
                    <th>V4 Audio Time<br><span style="font-weight:normal;font-size:11px;">(+14s offset)</span></th>
                    <th>Difference</th>
                    <th>Text</th>
                </tr>
            </thead>
            <tbody>
"""

# Add section headers and rows
current_section = None
for seg in display_segments:
    transcript_start = seg['start']
    transcript_end = seg['end']
    
    # Determine section
    if transcript_start < 100:
        section = "Beginning (First 50 segments)"
    elif 1430 <= transcript_start <= 1450:
        section = "Minute 24 (1440s)"
    elif 2870 <= transcript_start <= 2890:
        section = "Minute 48 (2880s)"
    else:
        continue
    
    # Add section header if new section
    if section != current_section:
        html += f'<tr><td colspan="5" class="section-header">{section}</td></tr>\n'
        current_section = section
    
    v3_start = transcript_start + v3_offset
    v3_end = transcript_end + v3_offset
    v4_start = transcript_start + v4_offset
    v4_end = transcript_end + v4_offset
    
    text = seg['text'][:100] + ('...' if len(seg['text']) > 100 else '')
    
    html += f"""
                <tr>
                    <td class="transcript-time">{transcript_start:.1f}s - {transcript_end:.1f}s</td>
                    <td class="v3-time">{v3_start:.1f}s - {v3_end:.1f}s</td>
                    <td class="v4-time">{v4_start:.1f}s - {v4_end:.1f}s</td>
                    <td style="font-weight:bold;color:#e67e22;">{offset_diff:.1f}s</td>
                    <td class="text-cell">{text}</td>
                </tr>
"""

html += """
            </tbody>
        </table>
    </div>

    <div style="margin-top: 20px; padding: 15px; background: white; border-radius: 8px;">
        <h3>How to Read This Table</h3>
        <ul>
            <li><strong>Transcript Time:</strong> The timestamp in the original transcript JSON</li>
            <li><strong>V3 Audio Time:</strong> Where V3 thinks this text appears in the audio file (transcript time + 46s)</li>
            <li><strong>V4 Audio Time:</strong> Where V4 thinks this text appears in the audio file (transcript time + 14s)</li>
            <li><strong>Difference:</strong> How far apart V3 and V4 disagree (always 32 seconds)</li>
        </ul>
        <p><strong>Example:</strong> The text "So ein Pinguin," appears at transcript time 1440.3s.</p>
        <ul>
            <li>V3 says it's at audio time <span style="color:#27ae60;">1486.3s</span> (1440.3 + 46.0)</li>
            <li>V4 says it's at audio time <span style="color:#e74c3c;">1454.3s</span> (1440.3 + 14.0)</li>
            <li>They disagree by <span style="color:#e67e22;">32.0 seconds</span></li>
        </ul>
        <p><strong>Only one can be correct!</strong> We need to verify which alignment is accurate.</p>
    </div>
</body>
</html>
"""

# Save HTML
output_path = "/tmp/fixed_alignment_test/v3_v4_comparison.html"
with open(output_path, 'w', encoding='utf-8') as f:
    f.write(html)

print(f"HTML comparison saved to: {output_path}")
print("\nOpen this file in a web browser to see the side-by-side comparison.")

# Made with Bob