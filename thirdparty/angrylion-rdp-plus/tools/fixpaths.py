#!/usr/bin/env python3
#
#   Dreadful hack for converting windows paths to unix paths in dependency
#   files output by clang++.exe so that it plays nice with WSL.
#

import os, sys

for root,dirs,files in os.walk(sys.argv[1]):
    for f in (f for f in files if f.endswith(".d")):
        with open(os.path.join(root,f), "r+") as file:
            content = file.read()
            newcontent = ""
            for line in content.split("\n"):
                if line.endswith("\\"):
                    line = line[:-1]
                    line = line.replace("\\", "/")
                    line = line + "\\"
                else:
                    line = line.replace("\\", "/")
                newcontent += line + "\n"
            file.seek(0)
            file.write(newcontent)
            file.truncate()
