#pragma once

#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"

class FEquipmentPluginModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	TArray<TUniquePtr<FAutoConsoleCommandWithWorldAndArgs>> ConsoleCommands;
};
