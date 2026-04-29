#include <filesystem>
#include <iostream>

#include <IFSelect_ReturnStatus.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFApp_Application.hxx>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: step_import_probe <input.step>\n";
        return 2;
    }

    const std::filesystem::path stepPath = argv[1];

    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    if (app.IsNull())
    {
        std::cerr << "Failed to get XCAF application\n";
        return 1;
    }

    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);
    if (doc.IsNull())
    {
        std::cerr << "Failed to create XCAF document\n";
        return 1;
    }

    STEPCAFControl_Reader reader;
    reader.SetColorMode(Standard_True);
    reader.SetNameMode(Standard_True);
    reader.SetLayerMode(Standard_False);
    reader.SetPropsMode(Standard_True);
    reader.SetMatMode(Standard_True);
    reader.SetGDTMode(Standard_False);
    reader.SetSHUOMode(Standard_True);

    const IFSelect_ReturnStatus status = reader.ReadFile(stepPath.string().c_str());
    if (status != IFSelect_RetDone)
    {
        std::cerr << "ReadFile failed\n";
        return 3;
    }

    if (!reader.Transfer(doc))
    {
        std::cerr << "Transfer failed\n";
        return 4;
    }

    std::cout << "Import OK\n";
    return 0;
}
