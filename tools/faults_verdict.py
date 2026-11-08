"""Expected fields from one real-device scenario, never combined across opens."""
def matches(result, expected):
    if not result or result == 'TIMEOUT' or '[exit ' in result:
        return False
    fields = result.split()
    return all(field in fields for field in expected.split())
