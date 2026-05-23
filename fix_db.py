import sqlite3
import os

db_path = os.path.expanduser(r'~\.conan2\.conan.db')

if os.path.exists(db_path):
    os.remove(db_path)
    print(f"Removed: {db_path}")
else:
    print(f"Not found: {db_path}")

conn = sqlite3.connect(db_path)
conn.execute("""
    CREATE TABLE IF NOT EXISTS recipes (
        reference TEXT PRIMARY KEY,
        last_access INTEGER
    )
""")
conn.execute("""
    CREATE TABLE IF NOT EXISTS packages (
        reference TEXT,
        package_id TEXT,
        last_access INTEGER,
        PRIMARY KEY (reference, package_id)
    )
""")
conn.execute("""
    CREATE TABLE IF NOT EXISTS users_remotes (
        user TEXT,
        remote_name TEXT,
        url TEXT,
        verify_ssl INTEGER,
        PRIMARY KEY (user, remote_name)
    )
""")
conn.commit()
conn.close()
print(f"Created new database: {db_path}")
