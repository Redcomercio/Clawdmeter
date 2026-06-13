"""conftest.py — adds the daemon directory to sys.path so bare imports like
`from claude_usage_daemon import ...` resolve when pytest runs from daemon/ or
from the repo root."""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
