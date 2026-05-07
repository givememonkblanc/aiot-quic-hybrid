import re
with open("src/components/wolfssl/src/quic.c", "r") as f:
    text = f.read()

pattern = re.compile(r'printf\([\s\S]*?plain\[3\]\);', re.MULTILINE)
text = pattern.sub('printf("QUIC ENCRYPT: plainlen=%d, first bytes: %02x %02x %02x %02x\\n", (int)plainlen, plain[0], plain[1], plain[2], plain[3]);', text)

with open("src/components/wolfssl/src/quic.c", "w") as f:
    f.write(text)
