#!/bin/sh
# Run manually inside the BoldonOS guest, not as the boot-time /etc/init.sh.
# Non-destructive diagnostics: one temporary file, then a bounded D-Bus test.
# No set -e: keep printing results even if a syscall fails.

PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
printf '%s\n' '=== BoldonOS guest diagnostic begin ==='
printf 'pid=%s\n' "$$"

probe="/tmp/boldon-io-$$.txt"
if [ ! -d /tmp ]; then
    printf '%s\n' 'IO SKIP: /tmp does not exist (not treating this as a write failure)'
else
    printf 'BOLDON_IO_%s\n' "$$" > "$probe"
    write_rc=$?
    printf 'IO write_rc=%s path=%s\n' "$write_rc" "$probe"
    if [ "$write_rc" -eq 0 ]; then
        content=$(cat "$probe" 2>&1)
        read_rc=$?
        printf 'IO read_rc=%s content=<%s>\n' "$read_rc" "$content"
        if [ "$read_rc" -eq 0 ] && [ "$content" = "BOLDON_IO_$$" ]; then
            printf '%s\n' 'IO PASS: write, close and reopen/read'
        else
            printf '%s\n' 'IO FAIL: file is empty, different, or unreadable'
        fi
    else
        printf '%s\n' 'IO FAIL: shell could not write temporary file'
    fi
    rm -f "$probe"
fi

printf '%s\n' '=== baseline tools ==='
for tool in dbus-run-session dbus-daemon timeout startplasma-wayland; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf '%s: available\n' "$tool"
    else
        printf '%s: missing\n' "$tool"
    fi
done

printf '%s\n' '=== D-Bus wrapper (bounded observation) ==='
if command -v timeout >/dev/null 2>&1 && command -v dbus-run-session >/dev/null 2>&1; then
    # Do not redirect the daemon's stdout into a second file: it would mask
    # whether the wrapper's inherited pipe or ordinary file I/O is failing.
    # Keep the test in the background: timeout itself may not be able to
    # interrupt a task blocked inside a guest kernel syscall.
    timeout -k 2 8 dbus-run-session -- /bin/echo DBUS_WRAPPER_OK &
    wrapper_pid=$!
    sleep 11
    if kill -0 "$wrapper_pid" 2>/dev/null; then
        printf '%s\n' 'DBUS HANG: wrapper/timeout still alive after 11 seconds'
        kill -KILL "$wrapper_pid" 2>/dev/null
        # Do not wait indefinitely; kernel-blocked descendants may remain.
    else
        wait "$wrapper_pid"
        printf 'DBUS wrapper_rc=%s (124 means timeout)\n' "$?"
    fi
else
    printf '%s\n' 'DBUS SKIP: timeout or dbus-run-session is unavailable'
fi

printf '%s\n' '=== recent guest syscall trace ==='
if [ -e /dev/xorgtrace ]; then
    cat /dev/xorgtrace
    printf 'TRACE read_rc=%s\n' "$?"
else
    printf '%s\n' 'TRACE SKIP: /dev/xorgtrace missing'
fi
printf '%s\n' '=== GPU capability summary ==='
if [ -e /dev/gpuinfo ]; then
    cat /dev/gpuinfo
else
    printf '%s\n' 'GPU SKIP: /dev/gpuinfo missing'
fi
printf '%s\n' '=== BoldonOS guest diagnostic end ==='
