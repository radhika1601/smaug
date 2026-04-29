import sys

def remove_target_triple(filename):
    with open(filename, 'r') as f:
        lines = f.readlines()
    with open(filename, 'w') as f:
        for line in lines:
            if not line.strip().startswith("target triple"):
                f.write(line)

if __name__ == "__main__":
    remove_target_triple(sys.argv[1])
