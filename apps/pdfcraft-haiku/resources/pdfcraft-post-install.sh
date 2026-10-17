#!/bin/sh
signature=application/x-vnd.storytold-PdfCraft
app=$(findpaths -p "$0" B_FIND_PATH_APPS_DIRECTORY PdfCraft 2>/dev/null)
[ -n "$app" ] && [ -e "$app" ] && mimeset -a -f "$app"
setmime -set application/pdf -short "PDF document" -extension pdf
setmime -dump application/pdf 2>/dev/null | grep -q -- -preferredAppSig \
    || setmime -set application/pdf -preferredAppSig "$signature"
exit 0
