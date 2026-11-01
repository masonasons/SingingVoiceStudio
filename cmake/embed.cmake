# A file as a C array, to be compiled into the program:
#
#   cmake -DIN=<file> -DOUT=<file.c> -DNAME=<symbol> -P embed.cmake
#
# gives `const unsigned char NAME[]` and `const unsigned long NAME_size`.
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR size "${digits} / 2")
# sixteen bytes to a line
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n" bytes "${bytes}")
file(WRITE ${OUT}.next
  "/* ${IN}, as embed.cmake writes it */\n"
  "const unsigned char ${NAME}[] = {\n${bytes}\n};\n"
  "const unsigned long ${NAME}_size = ${size};\n")
# written only when it changes
configure_file(${OUT}.next ${OUT} COPYONLY)
