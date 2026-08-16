#include "pynote/core/storage/migrations/registry.h"

#include "pynote/core/storage/migrations/v0001_initial.h"
#include "pynote/core/storage/migrations/v0002_data_policy_settings.h"
#include "pynote/core/storage/migrations/v0003_storage_invariants.h"
#include "pynote/core/storage/migrations/v0004_workspace_windows.h"
#include "pynote/core/storage/migrations/v0005_recency_sort_mode.h"
#include "pynote/core/storage/migrations/v0006_editor_split_sizes.h"
#include "pynote/core/storage/migrations/v0007_vertical_split_reset.h"
#include "pynote/core/storage/migrations/v0008_horizontal_split_reset.h"
#include "pynote/core/storage/migrations/v0009_preview_lines_default.h"

#include <iterator>

namespace pynote::core::storage::migrations
{
	namespace
	{
		// 파이썬 원본 __init__.py:33~42 의 MIGRATIONS 튜플이다. 등록 순서가 곧 적용 순서라
		// 이 배열의 순서 자체가 계약이다(러너의 pending 루프가 목록 순서에 의존한다).
		constexpr S_MIGRATION MIGRATIONS[] = {
			{ 1, &v0001::Migrate },
			{ 2, &v0002::Migrate },
			{ 3, &v0003::Migrate },
			{ 4, &v0004::Migrate },
			{ 5, &v0005::Migrate },
			{ 6, &v0006::Migrate },
			{ 7, &v0007::Migrate },
			{ 8, &v0008::Migrate },
			{ 9, &v0009::Migrate }
		};

		static_assert(std::size(MIGRATIONS) == 9, "원본 MIGRATIONS 튜플과 같은 아홉 본이어야 한다.");
	}

	std::span<const S_MIGRATION> Registry() noexcept
	{
		return(std::span<const S_MIGRATION>(MIGRATIONS));
	}

	int LatestSchemaVersion() noexcept
	{
		// 원본 LATEST_SCHEMA_VERSION = MIGRATIONS[-1][0] 이다(__init__.py:44).
		return(MIGRATIONS[std::size(MIGRATIONS) - 1].nVersion);
	}
}
