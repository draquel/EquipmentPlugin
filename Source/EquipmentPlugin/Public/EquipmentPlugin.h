#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"

/** The module's log category (EquipmentPlugin rule: one per module, with the module prefix). */
DECLARE_LOG_CATEGORY_EXTERN(LogEQP, Log, All);

class FEquipmentPluginModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	TArray<TUniquePtr<FAutoConsoleCommandWithWorldAndArgs>> ConsoleCommands;
};
