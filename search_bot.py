with open("headers_novos/dump.cs", "r", encoding="utf-8", errors="ignore") as f:
    content = f.read()

print("Contains DLENNMIMCDN:", "DLENNMIMCDN" in content)
