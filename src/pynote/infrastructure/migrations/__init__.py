from __future__ import annotations

from collections.abc import Callable

from pynote.infrastructure.migrations.v0001_initial import migrate as migrate_v1
from pynote.infrastructure.migrations.v0002_data_policy_settings import (
    migrate as migrate_v2,
)
from pynote.infrastructure.migrations.v0003_storage_invariants import (
    migrate as migrate_v3,
)
from pynote.infrastructure.migrations.v0004_workspace_windows import (
    migrate as migrate_v4,
)
from pynote.infrastructure.migrations.v0005_recency_sort_mode import (
    migrate as migrate_v5,
)
from pynote.infrastructure.migrations.v0006_editor_split_sizes import (
    migrate as migrate_v6,
)
from pynote.infrastructure.migrations.v0007_vertical_split_reset import (
    migrate as migrate_v7,
)
from pynote.infrastructure.migrations.v0008_horizontal_split_reset import (
    migrate as migrate_v8,
)
from pynote.infrastructure.migrations.v0009_preview_lines_default import (
    migrate as migrate_v9,
)
from pynote.infrastructure.migrations.v0010_card_file_bindings import (
    migrate as migrate_v10,
)

Migration = tuple[int, Callable[..., None]]

MIGRATIONS: tuple[Migration, ...] = (
    (1, migrate_v1),
    (2, migrate_v2),
    (3, migrate_v3),
    (4, migrate_v4),
    (5, migrate_v5),
    (6, migrate_v6),
    (7, migrate_v7),
    (8, migrate_v8),
    (9, migrate_v9),
    (10, migrate_v10),
)
LATEST_SCHEMA_VERSION = MIGRATIONS[-1][0]
