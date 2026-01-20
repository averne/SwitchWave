#!/bin/bash
# file-in file-out

if ! [ -x "$(command -v gimp)" ]; then
    exit 1
fi

IFS='-*' read -r NAME WIDTH HEIGHT COMP < <(basename $1 | cut -d '.' -f 1)

TMPPATH=$(mktemp --suffix=.dds)


gimp-console -n -i -c --batch-interpreter=plug-in-script-fu-eval -b "
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
    -b "(gimp-quit 0)"

dd if=$TMPPATH of=$2 bs=1 skip=128
