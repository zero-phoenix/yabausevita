# Contributing to YabauseVita

Thank you for your interest in contributing to YabauseVita! This document provides guidelines and instructions for contributing.

## Code of Conduct

- Be respectful and inclusive
- No harassment, discrimination, or abuse
- Focus on constructive feedback
- Help others learn and improve

## How to Contribute

### 1. Fork & Clone

```bash
# Fork the repository on GitHub
# Clone your fork
git clone https://github.com/YOUR_USERNAME/yabausevita.git
cd yabausevita
git remote add upstream https://github.com/zero-phoenix/yabausevita.git
```

### 2. Create a Branch

```bash
# Create a descriptive branch name
git checkout -b feature/add-gpu-support
# or
git checkout -b fix/crash-on-load
```

### 3. Make Changes

- Follow the existing code style
- Add comments for complex logic
- Keep commits atomic and descriptive
- Test your changes before pushing

### 4. Commit & Push

```bash
git add .
git commit -m "feat: Add GPU support for VDP1"
git push origin feature/add-gpu-support
```

### 5. Create Pull Request

1. Go to GitHub
2. Create PR from your branch
3. Describe what you changed and why
4. Link any related issues
5. Wait for review

## Code Style Guidelines

### C Code

- Use `snake_case` for variables and functions
- Use `UPPER_CASE` for constants
- Max line length: 100 characters
- Indent with 4 spaces
- Add function documentation comments

Example:
```c
/**
 * Emulate SH-2 CPU cycle
 * @param cpu Pointer to CPU state
 * @return Number of cycles executed
 */
int sh2_execute_cycle(sh2_cpu_t *cpu) {
    // Implementation
    return cycles;
}
```

### Python Scripts

- Follow PEP 8
- Use type hints where possible
- Add docstrings to functions
- Keep functions focused and small

## Areas Needing Help

- **GPU Pipeline**: VDP1/VDP2 implementation
- **Audio System**: Sound emulation
- **Game Compatibility**: Testing and fixes
- **Performance**: Optimization work
- **Documentation**: Guides and API docs
- **CI/CD**: GitHub Actions workflows

## Testing

Before submitting a PR:

1. Build locally:
   ```bash
   mkdir build && cd build
   cmake .. && make
   ```

2. Test with known working ROMs

3. Check for compiler warnings

4. Run any existing test suite

## Reporting Issues

### Bug Reports

Include:
- OS and architecture
- Build method used
- Game being tested
- Steps to reproduce
- Expected vs actual behavior
- Logs/error messages

### Feature Requests

Include:
- Clear description
- Use case/motivation
- Possible implementation approach
- Examples from other emulators

## Documentation

- Keep README.md updated
- Add docs for major features
- Include code examples
- Document known limitations

## Review Process

1. Automated checks (if any)
2. Code review by maintainers
3. Feedback and revisions
4. Approval and merge

## Questions?

- Open a [Discussion](https://github.com/zero-phoenix/yabausevita/discussions)
- Check existing [Issues](https://github.com/zero-phoenix/yabausevita/issues)
- Contact maintainers directly

## License

By contributing, you agree that your contributions will be licensed under the MIT License.

---

Thank you for helping make YabauseVita better! 🙏
