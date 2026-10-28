#pragma once

#pragma warning( push, 0 )
#include <tinyxml2.h>
#include <AntTweakBar.h>
#pragma warning( pop )
#include <unordered_map>
#include "SettingsFields.h"
class SettingsBlock;
class CD3D1XShader;
#include "D3D1XShaderDefines.h"
/*!
    \class SettingsHolder
    \brief Holds render hook settings in memory.
*/
class SettingsHolder
{
public:
    /// Global instance of settings holder.
    static SettingsHolder* Instance()
    {
        static SettingsHolder instance;
        return &instance;
    }
    /// Adds new settings block to settings holder
    void AddSettingBlock( SettingsBlock* block );
    /// Resets settings blocks to their default values
    void ResetSettings();
    /// Saves settings to default settings file
    void SaveSettings();
    /// Reloads settings from xml file
    void ReloadFile();
    /// Reloads shaders binded to settings blocks
    void ReloadShadersIfRequired();
    /// Initializes AntTweakBar GUI
    void InitGUI();
    /// Renders AntTweakBar GUI
    void DrawGUI();
    /// Returns true if AntTweakBar GUI is enabled
    bool IsGUIEnabled();
    /// Enables AntTweakBar GUI Display
    void EnableGUI();
    /// Disables AntTweakBar GUI Display
    void DisableGUI();
private:
    /// Loads settings from XML file
    void LoadSettings( const tinyxml2::XMLDocument &file );

    std::list<SettingsBlock*>	m_aSettingsBlocks;
    tinyxml2::XMLDocument			mSettingsFile;
    static std::string m_sSettingsFileName;
    bool m_bInitialized = false;
    bool m_bDrawTweakBar = false;
    TwBar* m_pGuiholder = nullptr;
};

/*!
    Settings block base class, used to hold various settings
*/
class SettingsBlock
{
public:
    SettingsBlock()
    {
        Reset();
        SettingsHolder::Instance()->AddSettingBlock( this );
    }
    /*
        \brief Saves contents of SettingsBlock to \ref XMLDocument.
        \returns Pointer to created XMLElement.
    */
    virtual tinyxml2::XMLElement* Save( tinyxml2::XMLDocument* doc );
    /*
        Loads contents of SettingsBlock from \ref XMLDocument.
    */
    virtual void Load( const tinyxml2::XMLDocument &doc );
    /*
        Resets contents of SettingsBlock to default values.
    */
    virtual void Reset();
    /*
        Initializes AntTweakBar GUI
    */
    virtual void InitGUI( TwBar* guiholder );
    /*
        Reloads shaders if required.
    */
    virtual void ReloadShaders();
    /*
        Returns value of integer settings field if this value exists 0 otherwise.
    */
    int GetInt( const std::string& name )
    {
        IntSField* field = dynamic_cast<IntSField*>( m_aFields[name] );
        return field != nullptr ? field->GetValue() : 0;
    }
    /*
        Returns value of unsigned integer settings field if this value exists 0 otherwise.
    */
    unsigned int GetUInt( const std::string& name )
    {
        UIntSField* field = dynamic_cast<UIntSField*>( m_aFields[name] );
        return field != nullptr ? field->GetValue() : 0;
    }
    /*
        Returns value of floating-point settings field if this value exists 0.0f otherwise.
    */
    float GetFloat( const std::string& name )
    {
        FloatSField* field = dynamic_cast<FloatSField*>( m_aFields[name] );
        return field != nullptr ? field->GetValue() : 0.0f;
    }
    /*
        Returns value of boolean settings field if this value exists false otherwise.
    */
    bool GetToggleField( const std::string& name )
    {
        ToggleSField* field = dynamic_cast<ToggleSField*>( m_aFields[name] );
        return field != nullptr ? field->GetValue() : false;
    }
    /// Name of settings block
    std::string m_sName;
    /// Pointers to shaders binded to this SettingsBlock
    std::vector<CD3D1XShader*> m_aShaderPointers;
    /// Shader define list for shaders binded to this SettingsBlock.
    CD3D1XShaderDefineList m_ShaderDefineList;

    bool m_bShaderReloadRequired = false;
protected:
    std::unordered_map<std::string, SettingsField*> m_aFields;
};

// Global shader settings block class, used to hold shader defenitions
class ShaderDefinesSettingsBlock : public SettingsBlock
{
public:
    ShaderDefinesSettingsBlock()
    {
        m_sName = "ShaderSettings";
        m_aFields["UsePBR"] = new ToggleSField( "UsePBR", true, false, true, m_sName, true );
        Reset();
    }
};
// Debug settings block class, used to hold debugging settings
class DebugSettingsBlock : public SettingsBlock
{
public:
    DebugSettingsBlock()
    {
        m_sName = "Debug";
        m_aFields["Windowed"] = new ToggleSField( "Windowed", true, false, false, m_sName, true );
        m_aFields["UseDefaultAdapter"] = new ToggleSField( "UseDefaultAdapter", true, false, false, m_sName, true );
        m_aFields["UseIdleHook"] = new ToggleSField( "UseIdleHook", false, true, false, m_sName, true );
        m_aFields["ShowPreformanceCounters"] = new ToggleSField( "ShowPreformanceCounters", false, false, false, m_sName );
        m_aFields["DebugMessaging"] = new ToggleSField( "DebugMessaging", false, false, false, m_sName, true );
        m_aFields["DebugRenderTarget"] = new ToggleSField( "DebugRenderTarget", false, true, false, m_sName );
        // HUD layer (CHudLayer): 2D drawn into its own layer, composited before Present; view 0 = normal,
        // 1 = frame without HUD, 2 = HUD layer alone over grey
        m_aFields["HudLayer"] = new ToggleSField( "HudLayer", false, false, false, m_sName, false );
        m_aFields["HudLayerView"] = new IntSField( "HudLayerView", false, false, false, m_sName, 0, 0, 2 );
        // frame generation bridge, step 1 (CFrameGenBridge): frames presented by RHFrameGen64.exe over the game window
        m_aFields["FrameGenBridge"] = new ToggleSField( "FrameGenBridge", true, false, false, m_sName, false );
        // Reflex through the bridge: sleep at frame start as Reflex decides (off = markers only)
        m_aFields["FrameGenReflexSleep"] = new ToggleSField( "FrameGenReflexSleep", true, false, false, m_sName, true );
        // the game waits for the helper to take its previous frame (no frames rendered just to be skipped)
        m_aFields["FrameGenBackPressure"] = new ToggleSField( "FrameGenBackPressure", false, false, false, m_sName, true );
        m_aFields["FrameGenBridgeVSync"] = new IntSField( "FrameGenBridgeVSync", true, false, false, m_sName, 0, 0, 1 );
        // NVIDIA Reflex on RenderHook's device (CNvReflex); Boost keeps GPU clocks up
        m_aFields["NvReflex"] = new ToggleSField( "NvReflex", false, false, false, m_sName, false );
        m_aFields["NvReflexBoost"] = new ToggleSField( "NvReflexBoost", false, false, false, m_sName, false );
        m_aFields["DebugLevel"] = new IntSField( "DebugLevel", true, false, false, m_sName, 0, 0, 2 );
        Reset();
    }
    virtual void Reset() override;
    virtual void InitGUI( TwBar* guiholder ) override;
public:
    unsigned int DebugRenderTargetNumber;
    std::vector<RwRaster*> DebugRenderTargetList;
};

extern DebugSettingsBlock gDebugSettings;
extern ShaderDefinesSettingsBlock gShaderDefineSettings;