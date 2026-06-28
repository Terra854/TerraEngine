#include "TransactionalMutationTestFixtures.h"

#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"

namespace TerraEngine::Tests {
	TransactionalMutationFixture MakeTransactionalMutationFixture()
	{
		ComponentRegistry::RegisterAllComponents();
		ObjectManager& objects = ObjectManager::instance();
		objects.clear();
		Object root = objects.createObject("TransactionRoot");
		return TransactionalMutationFixture{
			objects,
			root,
			objects.getUUID(root.EntityId())
		};
	}
}
