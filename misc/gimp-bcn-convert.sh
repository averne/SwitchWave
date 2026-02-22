#!/bin/bash
set -eo pipefail
# file-in file-out

if ! [ -x "$(command -v gimp)" ]; then
    echo "Error: gimp not found" >&2
    exit 1
fi

# serialize to avoid race conditions
exec 9>/tmp/.gimp-bcn-convert.lock
flock 9

IFS='-*' read -r NAME WIDTH HEIGHT COMP < <(basename $1 | cut -d '.' -f 1)

GIMP_MAJOR=$(gimp --version 2>&1 | grep -oP '\d+' | head -1)

if [ "$GIMP_MAJOR" -ge 3 ] 2>/dev/null; then
    # GIMP 3: uses gimp-console with keyword arguments and file-dds-export
    TMPPATH=$(mktemp --suffix=.dds)
    trap 'rm -f "$TMPPATH"' EXIT

    if ! gimp-console -n -i -c --batch-interpreter=plug-in-script-fu-eval -b "
        (let* (
                (image (car (file-svg-load
                    #:run-mode RUN-NONINTERACTIVE
                    #:file \"$1\"
                    #:width $WIDTH
                    #:height $HEIGHT
                    #:keep-ratio FALSE
                    #:paths \"no-import\")))
            )
            (file-dds-export
                #:run-mode RUN-NONINTERACTIVE
                #:image image
                #:file \"$TMPPATH\"
                #:options -1
                #:compression-format \"$COMP\"
                #:perceptual-metric FALSE
                #:format \"default\"
                #:save-type \"canvas\")
            (gimp-image-delete image)
        )" \
        -b "(gimp-quit 0)" 2>/dev/null; then
        echo "Error: GIMP failed to convert $1" >&2
        exit 1
    fi

    if [ ! -s "$TMPPATH" ]; then
        echo "Error: GIMP produced no output for $1" >&2
        exit 1
    fi

    dd if="$TMPPATH" of="$2" bs=1 skip=128 2>/dev/null
else
    # GIMP 2: uses gimp with positional arguments and file-dds-save
    case $COMP in
      "bc1") METHOD="1" ;;
      "bc2") METHOD="2" ;;
      "bc3") METHOD="3" ;;
      "bc4") METHOD="5" ;;
      "bc5") METHOD="6" ;;
       *) echo "Error: unknown compression format '$COMP'" >&2; exit 1 ;;
    esac

    gimp -i -b "
        (let* (
                (image (car (file-svg-load RUN-NONINTERACTIVE \"$1\" \"$1\" 90 $WIDTH $HEIGHT 0)))
                (drawable (car (gimp-image-get-active-layer image)))
            )
            (file-dds-save RUN-NONINTERACTIVE image drawable \"/dev/stdout\" \"/dev/stdout\" $METHOD 0 0 0 -1 0 0 0 0 0 0 0 0)
            (gimp-image-delete image)
        )" \
        -b "(gimp-quit 0)" 2>/dev/null \
        | dd of="$2" bs=1 skip=128 2>/dev/null
fi
