import re
with open("src/components/wolfssl/src/quic.c", "r") as f:
    text = f.read()

# Replace the broken printf with a valid one
text = re.sub(r'printf\("QUIC ENCRYPT: plainlen=%d, first bytes: %02x %02x %02x %02x\n.*?\);',
              'printf("QUIC ENCRYPT: plainlen=%d, first bytes: %02x %02x %02x %02x\\n", (int)plainlen, plain[0], plain[1], plain[2], plain[3]);',
              text, flags=re.DOTALL)

with open("src/components/wolfssl/src/quic.c", "w") as f:
    f.write(text)
