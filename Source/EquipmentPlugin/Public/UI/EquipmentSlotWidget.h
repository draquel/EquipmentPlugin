// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameplayTagContainer.h"
#include "Engine/StreamableManager.h"
#include "EquipmentSlotWidget.generated.h"

class UEquipmentManagerComponent;
class USizeBox;
class UVerticalBox;
class UOverlay;
class UImage;
class UTextBlock;

/** Fired when an equipment slot is clicked. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnEquipmentSlotClicked, FGameplayTag, SlotTag, UEquipmentManagerComponent*, EquipmentManager);

/**
 * Single equipment slot display widget.
 *
 * Shows the equipped item icon (or an empty placeholder) with a
 * label underneath showing the slot's display name (e.g., "Main Hand").
 */
UCLASS(BlueprintType, Blueprintable)
class EQUIPMENTPLUGIN_API UEquipmentSlotWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// --- Style ---

	/** Background brush for the slot. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EquipmentSlot|Style")
	FSlateBrush SlotBackgroundBrush;

	/** Brush shown when no item is equipped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EquipmentSlot|Style")
	FSlateBrush EmptySlotBrush;

	/** Size of the slot icon area in pixels. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EquipmentSlot|Style")
	float SlotSize = 64.f;

	// --- Delegates ---

	/** Fired when this slot is left-clicked. */
	UPROPERTY(BlueprintAssignable, Category = "EquipmentSlot|Events")
	FOnEquipmentSlotClicked OnSlotClicked;

	/** Fired when this slot is right-clicked. */
	UPROPERTY(BlueprintAssignable, Category = "EquipmentSlot|Events")
	FOnEquipmentSlotClicked OnSlotRightClicked;

	// --- API ---

	/** Bind this widget to an equipment manager and slot tag. */
	UFUNCTION(BlueprintCallable, Category = "EquipmentSlot")
	void InitSlot(UEquipmentManagerComponent* InEquipmentManager, FGameplayTag InSlotTag);

	/** Refresh the display from the equipment manager. */
	UFUNCTION(BlueprintCallable, Category = "EquipmentSlot")
	void RefreshSlot();

	/** Returns the slot tag this widget is bound to. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "EquipmentSlot")
	FGameplayTag GetSlotTag() const { return SlotTag; }

	/** Set or clear the held (grabbed) highlight on this slot. */
	UFUNCTION(BlueprintCallable, Category = "EquipmentSlot")
	void SetHeld(bool bInHeld);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	void BuildWidgetTree();

	UFUNCTION()
	void HandleEquipmentChanged();

	UPROPERTY()
	TObjectPtr<USizeBox> RootSizeBox;

	UPROPERTY()
	TObjectPtr<UImage> BackgroundImage;

	UPROPERTY()
	TObjectPtr<UImage> IconImage;

	UPROPERTY()
	TObjectPtr<UTextBlock> SlotNameText;

	UPROPERTY()
	TObjectPtr<UEquipmentManagerComponent> BoundEquipmentManager;

	FGameplayTag SlotTag;

	bool bIsHeld = false;

	TSharedPtr<FStreamableHandle> IconLoadHandle;
};
