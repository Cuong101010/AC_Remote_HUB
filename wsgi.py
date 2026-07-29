import os
import sys

# Tự động nạp đường dẫn thư mục dự án và backend vào Python Path
project_home = os.path.dirname(os.path.abspath(__file__))
if project_home not in sys.path:
    sys.path.insert(0, project_home)

backend_dir = os.path.join(project_home, "backend")
if backend_dir not in sys.path:
    sys.path.insert(0, backend_dir)

from backend.app import app as application
