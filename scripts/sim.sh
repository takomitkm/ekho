#!/bin/bash
# ekho-linux simulation harness.
#
# Measures the properties the streaming design is supposed to have, using the
# same definitions as the board-side TTS benchmark:
#
#   first_ms  append() -> first PCM a reader could take
#   gen_ms    total time inside synth4()
#   rtf       audio seconds per synthesis second
#   dropped   16-bit frames the PCM ring had to discard
#
# Sections 1-2 are correctness (the streaming path must produce the same audio
# as the one-shot path, and must be deterministic). Sections 3-5 are the
# streaming properties. Sections 6-8 are the other back ends.
set -u

SRC=${SRC:-/mnt/s/apps/llm/agentwork/ekho-linux}
OUT=${OUT:-$SRC/out}
SIM=${SIM:-/tmp/ekho-sim}
mkdir -p "$SIM"

export EKHO_DATA_PATH="$SRC/ekho-data"

cat > "$SIM/zh_long.txt" <<'EOF'
你好，这是在学校机器上用 MinGW 编译的 Ekho 十一点零版本测试。语音合成正常，这是普通话。
EOF
cat > "$SIM/zh_stream.txt" <<'EOF'
系统启动完成。第一项，电源状态正常。第二项，存储设备已挂载，剩余空间百分之八十七。第三项，网络链路已建立，延迟二十三毫秒。现在开始播放欢迎词。任务结束。
EOF
cat > "$SIM/en_mixed.txt" <<'EOF'
你好，这是用 MinGW 编译的 Ekho 十一点零版本测试。Windows 和 MSYS2 都可以正常工作。
EOF
cat > "$SIM/nums.txt" <<'EOF'
输出电压 12.5V，电流 2.4A，效率提升到 93.7%。CPU 频率 2.4GHz，内存 16GB。
EOF

hr() { printf '\n%s\n' "=== $* ==="; }

# Pull a field out of the driver's JSON without needing jq.
field() { sed -n "s/.*\"$2\":\([^,}]*\).*/\1/p" <<<"$1"; }

dur() { awk -v b="$(stat -c%s "$1" 2>/dev/null || echo 0)" \
             -v r="${2:-44100}" 'BEGIN{printf "%.3f", (b-44)/2/r}'; }

hr "1. correctness: streaming must not lose audio"
# Same text, same voice, three ways. The one-shot CLI is the reference; the
# streaming path splits at clause boundaries, which shifts the pauses at each
# join but must not drop syllables, so the durations should be within a few
# percent. `--no-segment` isolates the synth4() callback path from the
# segmentation on top of it.
for v in Mandarin Cantonese; do
  echo "  --- $v ---"
  "$OUT/ekho" -v "$v" -f "$SIM/zh_long.txt" -o "$SIM/k_oneshot.wav" >/dev/null 2>&1
  printf '    %-38s %9s B  %8s s\n' "one-shot CLI (reference)" \
    "$(stat -c%s "$SIM/k_oneshot.wav")" "$(dur "$SIM/k_oneshot.wav")"
  "$OUT/ekho-stream" -v "$v" -f "$SIM/zh_long.txt" --no-segment --json \
    -o "$SIM/k_whole.wav" >/dev/null 2>&1
  printf '    %-38s %9s B  %8s s\n' "streaming, one synth4 of everything" \
    "$(stat -c%s "$SIM/k_whole.wav")" "$(dur "$SIM/k_whole.wav")"
  "$OUT/ekho-stream" -v "$v" -f "$SIM/zh_long.txt" --json \
    -o "$SIM/k_seg.wav" >/dev/null 2>&1
  printf '    %-38s %9s B  %8s s\n' "streaming, clause-segmented" \
    "$(stat -c%s "$SIM/k_seg.wav")" "$(dur "$SIM/k_seg.wav")"
done

hr "1b. byte-chunked input must not lose characters"
# A byte-chunked caller splits multi-byte characters; the session holds the
# partial sequence over to the next call. Chunk sizes 1/3/6 all land inside a
# 3-byte CJK character at different phases and must give the same audio.
printf '  %-14s %10s %10s\n' chunk_B bytes diff_vs_whole
base=$(stat -c%s "$SIM/k_seg.wav" 2>/dev/null || echo 0)
for c in 1 2 3 4 6 7; do
  "$OUT/ekho-stream" -v Mandarin -f "$SIM/zh_long.txt" --chunk "$c" --json \
    -o "$SIM/k_c$c.wav" >/dev/null 2>&1
  b=$(stat -c%s "$SIM/k_c$c.wav")
  printf '  %-14s %10s %10s\n' "$c" "$b" "$((b - base))"
done

hr "2. determinism (same input three times)"
for i in 1 2 3; do
  "$OUT/ekho-stream" -v Mandarin -f "$SIM/zh_stream.txt" --chunk 6 --chunk-delay 5 \
    --json -o "$SIM/det_$i.wav" >/dev/null 2>&1
  printf '  run %d: %s  %s B\n' "$i" "$(md5sum "$SIM/det_$i.wav" | cut -c1-16)" \
    "$(stat -c%s "$SIM/det_$i.wav")"
done

hr "3. the streaming property: text arriving one token at a time"
# This is the case the design exists for. `--chunk-delay` stands in for an LLM
# emitting tokens; first_ms is then bounded by how long the first clause takes
# to arrive, and gen_ms shows how much redundant work each strategy does.
echo "  arrival: 12 bytes every 30 ms (about four Chinese characters)"
printf '  %-22s %10s %10s %10s %9s %8s\n' mode first_ms gen_ms total_ms rtf dropped
for mode in "--no-segment" ""; do
  label=$([ -z "$mode" ] && echo "clause-segmented" || echo "whole-buffer")
  r=$("$OUT/ekho-stream" -v Mandarin -f "$SIM/zh_stream.txt" \
      --chunk 12 --chunk-delay 30 --json $mode -o "$SIM/arr_$label.wav" 2>/dev/null)
  printf '  %-22s %10s %10s %10s %9s %8s\n' "$label" \
    "$(field "$r" first_ms)" "$(field "$r" gen_ms)" "$(field "$r" total_ms)" \
    "$(field "$r" rtf)" "$(field "$r" dropped_frames)"
done

hr "4. first-chunk latency against chunk size (no arrival delay)"
printf '  %-12s %10s %10s %10s %8s\n' chunk_B first_ms gen_ms segments dropped
for c in 6 12 24 48 0; do
  lbl=$([ "$c" = 0 ] && echo all || echo "$c")
  r=$("$OUT/ekho-stream" -v Mandarin -f "$SIM/zh_stream.txt" --chunk "$c" --json \
      -o "$SIM/cs.wav" 2>/dev/null)
  printf '  %-12s %10s %10s %10s %8s\n' "$lbl" "$(field "$r" first_ms)" \
    "$(field "$r" gen_ms)" "$(field "$r" segments)" "$(field "$r" dropped_frames)"
done

hr "5. backpressure: nothing is dropped, whatever the ring size"
# The producer blocks instead of discarding audio. A ring smaller than one
# segment must still lose zero frames -- it just makes synthesis slower.
printf '  %-12s %10s %10s %8s %10s\n' ring_ms segments gen_ms rtf dropped
for ms in 100 250 1000 4000 60000; do
  r=$("$OUT/ekho-stream" -v Mandarin -f "$SIM/zh_stream.txt" --ring-ms "$ms" --json \
      -o "$SIM/bp.wav" 2>/dev/null)
  printf '  %-12s %10s %10s %8s %10s\n' "$ms" "$(field "$r" segments)" \
    "$(field "$r" gen_ms)" "$(field "$r" rtf)" "$(field "$r" dropped_frames)"
done

hr "6. text normalisation"
"$OUT/ekho-stream" -v Mandarin -f "$SIM/nums.txt" --json -o "$SIM/nums.wav" 2>/dev/null
echo "  input : $(cat "$SIM/nums.txt")"
n=$(stat -c%s "$SIM/nums.wav")
"$OUT/ekho-stream" -v Mandarin -f "$SIM/nums.txt" --no-normalize --json \
  -o "$SIM/nums_raw.wav" >/dev/null 2>&1
printf '  audio : normalised %s B  vs  raw %s B  (+%s%%)\n' "$n" \
  "$(stat -c%s "$SIM/nums_raw.wav")" \
  "$(( (n - $(stat -c%s "$SIM/nums_raw.wav")) * 100 / $(stat -c%s "$SIM/nums_raw.wav") ))"

hr "7. English via espeak-ng"
r=$("$OUT/ekho-stream" -v Mandarin -f "$SIM/en_mixed.txt" --json \
    -o "$SIM/en.wav" 2>&1)
echo "  $r"
[ -s "$SIM/en.wav" ] && echo "  audio: $(stat -c%s "$SIM/en.wav") B" || echo "  audio: MISSING"

hr "8. peak RSS"
for spec in "ekho|-v Mandarin -f $SIM/zh_long.txt -o $SIM/rss.wav" \
            "ekho-stream|-v Mandarin -f $SIM/zh_stream.txt --json -o $SIM/rss2.wav" \
            "ekho-stream|-v Mandarin -f $SIM/zh_stream.txt --chunk 12 --chunk-delay 30 -o $SIM/rss3.wav"; do
  prog=${spec%%|*}; args=${spec#*|}
  kb=$(/usr/bin/time -f '%M' -o /tmp/rss.txt "$OUT/$prog" $args >/dev/null 2>&1; cat /tmp/rss.txt)
  printf '  %-14s %8s KB\n' "$prog" "$kb"
done
