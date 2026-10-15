// A caller-owned no-allocation witness distinguishes committed publication from
// rollback even if allocating an exception's diagnostic text itself fails.
#pragma once

#include "Scene/TransactionalMutation.h"

namespace TerraEngine::Scene {
	struct TransactionCommitOutcome {
		bool committed = false;
	};

	TERRAENGINE_API TransactionResult CommitTransaction(ObjectManager& objects,
		const Transaction& transaction, TransactionCommitOutcome& outcome);
}
