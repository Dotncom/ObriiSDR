#include "meshnodecli.h"
#include <QCoreApplication>
int main(int argc,char **argv){QCoreApplication app(argc,argv);return Mesh::runNodeService(app,app.arguments());}
