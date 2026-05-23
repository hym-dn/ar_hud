import sqlite3
import os

db_path = os.path.expanduser(r'~\.conan2\.conan.db')
print(f"DB path: {db_path}")
print(f"Exists: {os.path.exists(db_path)}")
print(f"Size: {os.path.getsize(db_path)}")

try:
    conn = sqlite3.connect(db_path)
    cursor = conn.execute("SELECT name FROM sqlite_master WHERE type='table'")
    tables = [r[0] for r in cursor]
    print(f"Tables: {tables}")

    for table in tables:
        cursor = conn.execute(f"SELECT count(*) FROM [{table}]")
        count = cursor.fetchone()[0]
        print(f"  {table}: {count} rows")

    conn.execute("PRAGMA integrity_check")
    print("Integrity: OK")

    try:
        conn.execute("UPDATE recipes SET last_access=0 WHERE 0=1")
        print("Write test: OK")
    except Exception as e:
        print(f"Write test FAILED: {e}")

    conn.close()
except Exception as e:
    print(f"Error: {e}")
