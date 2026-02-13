// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "EquipmentPanelWidget.generated.h"

class UEquipmentManagerComponent;
class UEquipmentSlotWidget;
class UBorder;
class UVerticalBox;
class UTextBlock;

/**
 * Floating equipment panel widget.
 *
 * Shows all configured equipment slots vertically. Toggle-visible
 * alongside the inventory panel when the player opens inventory UI.
 */
UCLASS(BlueprintType, Blueprintable)
class EQUIPMENTPLUGIN_API UEquipmentPanelWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// --- Style ---

	/** Background brush for the panel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EquipmentPanel|Style")
	FSlateBrush PanelBackgroundBrush;

	/** Background tint color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EquipmentPanel|Style")
	FLinearColor PanelBackgroundTint = FLinearColor(0.05f, 0.05f, 0.1f, 0.85f);

	/** Title text displayed at the top of the panel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EquipmentPanel|Style")
	FText PanelTitle = NSLOCTEXT("EquipmentPanel", "Title", "Equipment");

	/** Padding around the panel content. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EquipmentPanel|Style")
	FMargin PanelPadding = FMargin(8.f);

	/** Spacing between equipment slot widgets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EquipmentPanel|Style")
	float SlotSpacing = 4.f;

	/** Override class for slot widgets (for Blueprint skinning). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "EquipmentPanel|Style")
	TSubclassOf<UEquipmentSlotWidget> SlotWidgetClass;

	// --- API ---

	/** Initialize the panel from an equipment manager's available slots. */
	UFUNCTION(BlueprintCallable, Category = "EquipmentPanel")
	void InitPanel(UEquipmentManagerComponent* InEquipmentManager);

	/** Refresh all slot widgets. */
	UFUNCTION(BlueprintCallable, Category = "EquipmentPanel")
	void RefreshAllSlots();

protected:
	virtual void NativeOnInitialized() override;

private:
	void BuildWidgetTree();

	UPROPERTY()
	TObjectPtr<UBorder> RootBorder;

	UPROPERTY()
	TObjectPtr<UTextBlock> TitleText;

	UPROPERTY()
	TObjectPtr<UVerticalBox> SlotContainer;

	UPROPERTY()
	TArray<TObjectPtr<UEquipmentSlotWidget>> SlotWidgets;

	UPROPERTY()
	TObjectPtr<UEquipmentManagerComponent> BoundEquipmentManager;
};
