"""One analysis-checked entry point for the current browser AVC candidates."""

_CANDIDATE_ENGINES = (
    "chromium",
    "firefox",
    "webkit",
)


def browser_h264_qualification_matrix(name, qualifications):
    """Defines the opt-in H.264 candidate-browser qualification matrix.

    This inventory deliberately says nothing about which engines the product
    supports. It only makes the three existing candidate qualifications
    runnable through one Bazel target. Keeping the expected engine names here
    makes deleting or misspelling a matrix member an analysis error rather than
    a silently narrower release check.

    Args:
      name: Name of the generated test_suite.
      qualifications: Mapping from candidate engine name to its test label.
    """
    actual_engines = sorted(qualifications.keys())
    expected_engines = sorted(_CANDIDATE_ENGINES)
    if actual_engines != expected_engines:
        fail(
            "%s must contain exactly the current browser qualification candidates; " % name +
            "expected %s, got %s" % (expected_engines, actual_engines),
        )

    tests = []
    for engine in _CANDIDATE_ENGINES:
        target = qualifications[engine]
        if type(target) != "string" or not target:
            fail("%s qualification must be a non-empty target label" % engine)
        tests.append(target)

    native.test_suite(
        name = name,
        tests = tests,
        tags = [
            "local",
            "manual",
        ],
    )
