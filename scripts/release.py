"""Build the single installable ZIP from a tested local build."""
from package import main


if __name__ == "__main__":
    main(require_tests=True)
