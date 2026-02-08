#pragma once

#include "Modules/ModuleManager.h"

class FEquipmentGASIntegrationModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
