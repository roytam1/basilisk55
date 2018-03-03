# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import configobj
import sys
import os
import re
from StringIO import StringIO

script_name = os.path.basename(__file__)

try:
    (file, section, key) = sys.argv[1:]
except ValueError:
    print "{0}: Usage: printconfigsetting.py <file> <section> <setting>".format(script_name)
    sys.exit(1)

if os.path.exists(file):
  with open(file) as fh:
      content = re.sub('^\s*;', '#', fh.read(), flags=re.M)
else:
  print >>sys.stderr, "{0}: warning: File {1} not found.".format(script_name, file)
  sys.exit(0)

c = configobj.ConfigObj(StringIO(content))

try:
    s = c[section]
except KeyError:
    print >>sys.stderr, "{0}: error: Section [{1}] not found.".format(script_name, section)
    sys.exit(1)

try:
    print s[key]
except KeyError:
    print >>sys.stderr, "{0}: error: Key {1} not found.".format(script_name, key)
    sys.exit(1)
