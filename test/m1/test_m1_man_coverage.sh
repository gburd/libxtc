#!/bin/sh
# test/m1/test_m1_man_coverage.sh -- D7
# Every internal __os_<topic> module has a man3 page that mentions
# every function declared in its header.

set -eu
: "${XTC_SRC_DIR:?}"

MANDIR="$XTC_SRC_DIR/man/man3"

# Map module -> header.
for mod in os_atomic os_alloc os_thread os_tls os_mutex os_time; do
	# Atomics, tls live in os_atomic.h or os_thread.h respectively;
	# we group them: one man page per logical group.
	case "$mod" in
		os_atomic) hdr="$XTC_SRC_DIR/src/inc/os_atomic.h" ;;
		os_alloc)  hdr="$XTC_SRC_DIR/src/inc/os_alloc.h"  ;;
		os_thread) hdr="$XTC_SRC_DIR/src/inc/os_thread.h" ;;
		os_tls)    hdr="$XTC_SRC_DIR/src/inc/os_thread.h" ;;
		os_mutex)  hdr="$XTC_SRC_DIR/src/inc/os_thread.h" ;;
		os_time)   hdr="$XTC_SRC_DIR/src/inc/os_time.h"   ;;
	esac

	page="$MANDIR/__$mod.3"
	if [ ! -f "$page" ]; then
		echo "  [D7] FAIL: $page missing for module $mod" >&2
		exit 1
	fi

	# Pull every function name __os_<mod-suffix>_<...> from the header.
	#
	# For the os_atomic module the names use suffix-typed variants
	# (__os_atomic_load_i32, etc.) -- we only require the *base* name
	# (__os_atomic_load) be mentioned by the page.
	case "$mod" in
		os_atomic)
			pat='^[A-Za-z_].*__os_(atomic|pause)'
			# extract the verb-or-pause root, dropping suffix typing
			fns=$(grep -oE '__os_(atomic_(load|store|cas|fetch_add|fence)|pause)' "$hdr" \
				| sort -u)
			;;
		os_tls)
			fns=$(grep -oE '__os_tls_[a-z_]+' "$hdr" | sort -u)
			;;
		os_thread)
			fns=$(grep -oE '__os_thread_[a-z_]+' "$hdr" | sort -u)
			;;
		os_mutex)
			fns=$(grep -oE '__os_(mutex|rwlock|cond|sem)_[a-z_]+' "$hdr" | sort -u)
			;;
		os_alloc)
			fns=$(grep -oE '__os_(malloc|calloc|realloc|free|strdup|aligned_alloc|alloc_set_hook|alloc_get_hook)' "$hdr" | sort -u)
			;;
		os_time)
			fns=$(grep -oE '__os_(clock_mono|clock_real|sleep_ns)' "$hdr" | sort -u)
			;;
	esac

	if [ -z "$fns" ]; then
		echo "  [D7] FAIL: no functions extracted from $hdr for $mod" >&2
		exit 1
	fi

	for fn in $fns; do
		if ! grep -q "$fn" "$page"; then
			echo "  [D7] FAIL: $page does not mention $fn" >&2
			exit 1
		fi
	done
	(void=$mod)
done

echo "  [D7] OK: every __os_* module has a man page covering its API"
