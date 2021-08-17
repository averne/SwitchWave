# file-in file-out

if ! [ -x "$(command -v gimp)" ]; then
    exit 1
fi

IFS='-*' read -r NAME WIDTH HEIGHT COMP < <(basename $1 | cut -d '.' -f 1)

case $COMP in
  "bc1") METHOD="1" ;;
  "bc2") METHOD="2" ;;
  "bc3") METHOD="3" ;;
  "bc4") METHOD="5" ;;
  "bc5") METHOD="6" ;;
   *) echo "Unknown compression format"; exit 1 ;;
esac

gimp -i -b "
    (let* (
            (image (car (file-svg-load RUN-NONINTERACTIVE \"$1\" \"$1\" 90 $WIDTH $HEIGHT 0)))
            (drawable (car (gimp-image-get-active-layer image)))
        )
        (file-dds-save RUN-NONINTERACTIVE image drawable \"/dev/stdout\" \"/dev/stdout\" $METHOD 0 0 0 -1 0 0 0 0 0 0 0 0)
        (gimp-image-delete image)
    )" \
    -b "(gimp-quit 0)" \
    | dd of=$2 bs=1 skip=128
