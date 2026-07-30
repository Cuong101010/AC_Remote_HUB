import os
import sys

# Đảm bảo thư mục backend đứng đầu sys.path để import storage.py và app.py chuẩn xác
project_home = os.path.dirname(os.path.abspath(__file__))
backend_dir = os.path.join(project_home, "backend")

if backend_dir not in sys.path:
    sys.path.insert(0, backend_dir)

if project_home not in sys.path:
    sys.path.insert(0, project_home)

from app import app as application
