#include "pynote/core/storage/migrations/registry.h"

#include "pynote/core/storage/migrations/v0001_initial.h"

#include <iterator>

namespace pynote::core::storage::migrations
{
	namespace
	{
		// 파이썬 원본 __init__.py:33~42 의 MIGRATIONS 튜플이다. 등록 순서가 곧 적용 순서라
		// 이 배열의 순서 자체가 계약이다(러너의 pending 루프가 목록 순서에 의존한다).
		// T-R1 시점에는 v0001 한 본만 이식돼 있고 T-R2 가 v0002~v0009 를 잇는다.
		constexpr S_MIGRATION MIGRATIONS[] = {
			{ 1, &v0001::Migrate }
		};
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
