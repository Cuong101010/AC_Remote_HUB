import os
import sys

# Nạp cả thư mục dự án và thư mục backend vào sys.path
project_home = os.path.dirname(os.path.abspath(__file__))
backend_dir = os.path.join(project_home, "backend")

for path in [backend_dir, project_home]:
    if path not in sys.path:
        sys.path.insert(0, path)

try:
    from app import app as application
except ImportError:
    from backend.app import app as application
