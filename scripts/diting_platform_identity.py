"""Validate a private, stable software platform UUID for the Diting ADT."""
import re
import uuid


def platform_identity(value):
    if not isinstance(value, str) or not re.fullmatch(
            r'[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}', value):
        raise ValueError('platform_uuid must be a canonical, nonzero UUID in ignored local.toml')
    identity = uuid.UUID(value)
    if not identity.int:
        raise ValueError('platform_uuid must not be the null UUID')
    return str(identity).encode('ascii') + b'\0'
