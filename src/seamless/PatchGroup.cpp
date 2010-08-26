/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2010 Pelican Ventures, Inc.
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <seamless/PatchGroup>

#include <osg/Array>
#include <osg/Geometry>
#include <osg/Notify>
#include <osg/Transform>
#include <osgDB/FileNameUtils>
#include <osgDB/Registry>

#include <seamless/Patch>
#include <seamless/PatchSet>

namespace seamless
{
using namespace std;
using namespace osg;

PatchGroup::PatchGroup()
{
    setRangeMode(LOD::PIXEL_SIZE_ON_SCREEN);
}

PatchGroup::PatchGroup(const PatchGroup& rhs, const CopyOp& copyop)
    : PagedLOD(rhs, copyop)
{
    setRangeMode(LOD::PIXEL_SIZE_ON_SCREEN);
}

PatchGroup::~PatchGroup()
{
}

void PatchGroup::getPatchExtents(Vec2d& lowerLeft, Vec2d& upperRight) const
{
    const PatchOptions* poptions
        = dynamic_cast<const PatchOptions*>(getDatabaseOptions());
    if (!poptions)
    {
        lowerLeft = Vec2d(0.0, 0.0);
        upperRight = Vec2d(1.0, 1.0);
    }
    else
    {
        poptions->getPatchExtents(lowerLeft, upperRight);
    }
}

// Child 0 is the patch for this LOD. Child 1 is a group of the 4
// patches for the next level.

// Copied and edited from PagedLOD::traverse.
void PatchGroup::traverse(NodeVisitor& nv)
{
    // set the frame number of the traversal so that external nodes can find out how active this
    // node is.
    if (nv.getFrameStamp() &&
        nv.getVisitorType()==osg::NodeVisitor::CULL_VISITOR)
    {
        setFrameNumberOfLastTraversal(nv.getFrameStamp()->getFrameNumber());
    }

    double timeStamp = nv.getFrameStamp()?nv.getFrameStamp()->getReferenceTime():0.0;
    int frameNumber = nv.getFrameStamp()?nv.getFrameStamp()->getFrameNumber():0;
    bool updateTimeStamp = nv.getVisitorType()==osg::NodeVisitor::CULL_VISITOR;

    switch(nv.getTraversalMode())
    {
    case(NodeVisitor::TRAVERSE_ALL_CHILDREN):
        std::for_each(_children.begin(),_children.end(),NodeAcceptOp(nv));
        break;
    case(NodeVisitor::TRAVERSE_ACTIVE_CHILDREN):
    {
        Vec3 eye = nv.getViewPoint();
        Patch* patch = 0;
        if (_children.empty())
            return;
        patch = dynamic_cast<Patch*>(_children[0].get());
        if (!patch)
        {
            Transform* tform = dynamic_cast<Transform*>(_children[0].get());
            if (!tform || tform->getNumChildren() == 0)
                return;
            Matrix localMat;
            tform->computeWorldToLocalMatrix(localMat, &nv);
            eye = eye * localMat;
            patch = dynamic_cast<Patch*>(tform->getChild(0));
            if (!patch)
                return;
        }

        float epsilon = patch->getPatchError(eye);

        int lastChildTraversed = -1;
        bool needToLoadChild = false;
        // Range list is set up so that the error is [0,1] for the
        // patch  at this level and (1, 1e10) for the next level.
        for(unsigned int i=0;i<_rangeList.size();++i)
        {
            if (_rangeList[i].first <= epsilon
                && epsilon < _rangeList[i].second)
            {
                if (i<_children.size())
                {
                    if (updateTimeStamp)
                    {
                        _perRangeDataList[i]._timeStamp=timeStamp;
                        _perRangeDataList[i]._frameNumber=frameNumber;
                    }

                    _children[i]->accept(nv);
                    lastChildTraversed = (int)i;
                }
                else
                {
                    needToLoadChild = true;
                }
            }
        }

        if (needToLoadChild)
        {
            unsigned int numChildren = _children.size();

            // select the last valid child.
            if (numChildren>0 && ((int)numChildren-1)!=lastChildTraversed)
            {
                if (updateTimeStamp)
                {
                    _perRangeDataList[numChildren-1]._timeStamp=timeStamp;
                    _perRangeDataList[numChildren-1]._frameNumber=frameNumber;
                }
                _children[numChildren-1]->accept(nv);
            }

            // now request the loading of the next unloaded child.
            if (!_disableExternalChildrenPaging &&
                nv.getDatabaseRequestHandler() &&
                numChildren<_perRangeDataList.size())
            {
                // compute priority from where abouts in the required range the distance falls.
                float priority = (_rangeList[numChildren].second - epsilon)/(_rangeList[numChildren].second-_rangeList[numChildren].first);

                // invert priority for PIXEL_SIZE_ON_SCREEN mode
                priority = -priority;

                // modify the priority according to the child's priority offset and scale.
                priority = _perRangeDataList[numChildren]._priorityOffset + priority * _perRangeDataList[numChildren]._priorityScale;

                if (_databasePath.empty())
                {
                    nv.getDatabaseRequestHandler()->requestNodeFile(_perRangeDataList[numChildren]._filename,this,priority,nv.getFrameStamp(), _perRangeDataList[numChildren]._databaseRequest, _databaseOptions.get());
                }
                else
                {
                    // prepend the databasePath to the child's filename.
                    nv.getDatabaseRequestHandler()->requestNodeFile(_databasePath+_perRangeDataList[numChildren]._filename,this,priority,nv.getFrameStamp(), _perRangeDataList[numChildren]._databaseRequest, _databaseOptions.get());
                }
            }

        }
        break;
    }
    default:
        break;
    }
}

// Pseudo loader for patch group

class ReaderWriterPatchGroup : public osgDB::ReaderWriter
{
public:
    ReaderWriterPatchGroup()
    {
        supportsExtension("tengpatch", "patch pseudo loader");
    }

    virtual const char* className() const
    {
        return "patch pseudo loader";
    }

    virtual osgDB::ReaderWriter::ReadResult
    readNode(const string& fileName,
             const osgDB::ReaderWriter::Options* options) const
    {
        string ext = osgDB::getFileExtension(fileName);
        if (!acceptsExtension(ext))
            return osgDB::ReaderWriter::ReadResult::FILE_NOT_HANDLED;
        Vec2d lowerLeft(0.0, 1.0);
        Vec2d upperRight(1.0, 1.0);
        const PatchOptions* poptions
            = dynamic_cast<const PatchOptions*>(options);
        if (!poptions)
        {
            OSG_FATAL
                << "PatchGroup reader: Options object is not PatchOptions";
            return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
        }
        PatchSet* pset = poptions->getPatchSet();
        Group* result = new Group;
        for (int i = 0; i < 4; ++i)
            result->addChild(pset->createChild(poptions, i));
        return result;
    }
};

REGISTER_OSGPLUGIN(tengpatch, ReaderWriterPatchGroup)

PatchOptions::PatchOptions()
    : _lowerLeft(0.0, 0.0), _upperRight(1.0, 1.0), _level(0)
{
}

PatchOptions::PatchOptions(const std::string& str)
    : osgDB::Options(str), _lowerLeft(0.0, 0.0), _upperRight(1.0, 1.0),
      _level(0)
{
}

PatchOptions::PatchOptions(const PatchOptions& rhs, const CopyOp& copyop)
    : osgDB::Options(rhs, copyop), _lowerLeft(rhs._lowerLeft),
      _upperRight(rhs._upperRight), _level(rhs._level)
{
    _patchSet = static_cast<PatchSet*>(copyop(rhs._patchSet.get()));
}

PatchOptions::~PatchOptions()
{
}

void PatchOptions::setPatchSet(PatchSet* patchSet)
{
    _patchSet = patchSet;
}

PatchSet* PatchOptions::getPatchSet() const
{
    return _patchSet.get();
}


}
