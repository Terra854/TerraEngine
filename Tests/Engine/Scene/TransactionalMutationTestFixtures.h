#pragma once

#include "Core/Object/Object.h"
#include "Utilities/UUID.h"

class ObjectManager;

namespace TerraEngine::Tests {
	struct TransactionalMutationFixture {
		ObjectManager& objects;
		Object root;
		TerraEngine::UUID rootId = TerraEngine::EMPTY_UUID;
	};

	TransactionalMutationFixture MakeTransactionalMutationFixture();
}
