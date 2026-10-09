#pragma once

#include "Modules/ModuleManager.h"

class AActor;
class UAbilitySystemComponent;

class FEquipmentGASIntegrationModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * The ability system component that equipment on this actor grants to. Resolves through
	 * IAbilitySystemInterface first (the ASC may live on another actor, e.g. a player state),
	 * then falls back to a component on the actor itself.
	 * @param Owner Actor owning the equipment manager. Null-safe.
	 */
	EQUIPMENTGASINTEGRATION_API static UAbilitySystemComponent* ResolveAbilitySystemComponent(AActor* Owner);
};
