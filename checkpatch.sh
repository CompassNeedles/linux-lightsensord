git diff $(git log --pretty=oneline | tail -1 | sed 's/ .*$//') -- '*.c' '*.h' '*.S' > srcdiff.txt
checkpatch.pl --ignore FILE_PATH_CHANGES -terse --no-signoff -no-tree srcdiff.txt > checkout.txt
