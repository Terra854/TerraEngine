#pragma once
class SystemsBase {
public:
	SystemsBase() {};
	virtual ~SystemsBase() = default;

	// System Initialization
	virtual void Initialize() = 0;

	// System Update
	virtual void Update() = 0;

	// System freeing resources
	virtual void shutdown() = 0;

	//For Debugging, getting System Name
	virtual std::string SystemName() { return "Unnamed system (If you are seeing this, you need to implement SystemName() inside the system)"; };
};