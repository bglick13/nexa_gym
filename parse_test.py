import os


def main():
    fname = "./qa/rpc-tests/wallet.py"
    with open(fname, "r") as f:
        code = f.read()
    functions = code.split("def ")[1:]

    puzzles = []
    solutions = []
    for func in functions:
        if "assert" not in func:
            continue
        puzzle = "def "
        for line in func.split("\n"):
            if line.strip().startswith("assert"):
                puzzles.append(puzzle)
                solutions.append(line)
            else:
                puzzle += line
                puzzle += "\n"

    print(puzzles[-1])
    print(solutions[-1])
    print(len(solutions))


if __name__ == "__main__":
    main()
