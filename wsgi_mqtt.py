import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'backend_mqtt'))

from app_mqtt import app as application

if __name__ == '__main__':
    application.run()
