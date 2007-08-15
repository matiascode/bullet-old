
#include "SpuGatheringCollisionTask.h"

#include "../SpuDoubleBuffer.h"

#include "../SpuCollisionTaskProcess.h"
#include "../SpuGatheringCollisionDispatcher.h" //for SPU_BATCHSIZE_BROADPHASE_PAIRS

#include "BulletCollision/BroadphaseCollision/btBroadphaseProxy.h"
#include "../SpuContactManifoldCollisionAlgorithm.h"
#include "BulletCollision/CollisionDispatch/btCollisionObject.h"
#include "SpuContactResult.h"
#include "BulletCollision/CollisionShapes/btOptimizedBvh.h"
#include "BulletCollision/CollisionShapes/btTriangleIndexVertexArray.h"
#include "BulletCollision/CollisionShapes/btSphereShape.h"

#include "BulletCollision/CollisionShapes/btCapsuleShape.h"

#include "BulletCollision/CollisionShapes/btConvexShape.h"
#include "BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h"
#include "BulletCollision/CollisionShapes/btConvexHullShape.h"
#include "BulletCollision/CollisionShapes/btCompoundShape.h"

#include "SpuMinkowskiPenetrationDepthSolver.h"
#include "SpuGjkPairDetector.h"
#include "SpuVoronoiSimplexSolver.h"

#include "SpuLocalSupport.h" //definition of SpuConvexPolyhedronVertexData

#ifdef USE_SN_TUNER
#include <LibSN_SPU.h>
#endif //USE_SN_TUNER

#ifdef WIN32

#define IGNORE_ALIGNMENT 1

#define spu_printf printf
#include <stdio.h>
#endif

#define MAX_SHAPE_SIZE 256

//int gNumConvexPoints0=0;



///Make sure no destructors are called on this memory
struct	CollisionTask_LocalStoreMemory
{

	ATTRIBUTE_ALIGNED16(char	bufferProxy0[16]);
	ATTRIBUTE_ALIGNED16(char	bufferProxy1[16]);

	ATTRIBUTE_ALIGNED16(btBroadphaseProxy*	gProxyPtr0);
	ATTRIBUTE_ALIGNED16(btBroadphaseProxy*	gProxyPtr1);

	ATTRIBUTE_ALIGNED16(btCollisionObject	gColObj0);
	ATTRIBUTE_ALIGNED16(btCollisionObject	gColObj1);

	DoubleBuffer<unsigned char, MIDPHASE_WORKUNIT_PAGE_SIZE> g_workUnitTaskBuffers;
	ATTRIBUTE_ALIGNED16(btBroadphasePair	gBroadphasePairs[SPU_BATCHSIZE_BROADPHASE_PAIRS]);


	//SpuContactManifoldCollisionAlgorithm	gSpuContactManifoldAlgo;
	//ATTRIBUTE_ALIGNED16(char	gSpuContactManifoldAlgo[sizeof(SpuContactManifoldCollisionAlgorithm)+128]);

	SpuContactManifoldCollisionAlgorithm	gSpuContactManifoldAlgo;

	SpuContactManifoldCollisionAlgorithm*	getlocalCollisionAlgorithm()
	{
		return (SpuContactManifoldCollisionAlgorithm*)&gSpuContactManifoldAlgo;

	}
	btPersistentManifold	gPersistentManifold;

	ATTRIBUTE_ALIGNED16(char	gCollisionShape0[MAX_SHAPE_SIZE]);
	ATTRIBUTE_ALIGNED16(char	gCollisionShape1[MAX_SHAPE_SIZE]);

	ATTRIBUTE_ALIGNED16(int	spuIndices[16]);

	ATTRIBUTE_ALIGNED16(btOptimizedBvh	gOptimizedBvh);
	ATTRIBUTE_ALIGNED16(btTriangleIndexVertexArray	gTriangleMeshInterface);
	///only a single mesh part for now, we can add support for multiple parts, but quantized trees don't support this at the moment 
	ATTRIBUTE_ALIGNED16(btIndexedMesh	gIndexMesh);

#define MAX_SPU_SUBTREE_HEADERS 32
	//1024
	ATTRIBUTE_ALIGNED16(btBvhSubtreeInfo	gSubtreeHeaders[MAX_SPU_SUBTREE_HEADERS]);
	ATTRIBUTE_ALIGNED16(btQuantizedBvhNode	gSubtreeNodes[MAX_SUBTREE_SIZE_IN_BYTES/sizeof(btQuantizedBvhNode)]);

	SpuConvexPolyhedronVertexData convexVertexData;

	// Compound data
#define MAX_SPU_COMPOUND_SUBSHAPES 16
	ATTRIBUTE_ALIGNED16(btCompoundShapeChild gSubshapes[MAX_SPU_COMPOUND_SUBSHAPES*2]);
	ATTRIBUTE_ALIGNED16(char gSubshapeShape[MAX_SPU_COMPOUND_SUBSHAPES*2][MAX_SHAPE_SIZE]);
	
};



#ifdef WIN32
void* createCollisionLocalStoreMemory()
{
	return new CollisionTask_LocalStoreMemory;
};


#elif defined(__CELLOS_LV2__)

ATTRIBUTE_ALIGNED16(CollisionTask_LocalStoreMemory	gLocalStoreMemory);

void* createCollisionLocalStoreMemory()
{
	return &gLocalStoreMemory;
}
#endif


void	ProcessSpuConvexConvexCollision(SpuCollisionPairInput* wuInput, CollisionTask_LocalStoreMemory* lsMemPtr, SpuContactResult& spuContacts);


inline bool spuTestQuantizedAabbAgainstQuantizedAabb(const unsigned short int* aabbMin1,const unsigned short int* aabbMax1,const unsigned short int* aabbMin2,const unsigned short int*  aabbMax2)
{
	bool overlap = true;
	overlap = (aabbMin1[0] > aabbMax2[0] || aabbMax1[0] < aabbMin2[0]) ? false : overlap;
	overlap = (aabbMin1[2] > aabbMax2[2] || aabbMax1[2] < aabbMin2[2]) ? false : overlap;
	overlap = (aabbMin1[1] > aabbMax2[1] || aabbMax1[1] < aabbMin2[1]) ? false : overlap;
	return overlap;
}


void	spuWalkStacklessQuantizedTree(btNodeOverlapCallback* nodeCallback,unsigned short int* quantizedQueryAabbMin,unsigned short int* quantizedQueryAabbMax,const btQuantizedBvhNode* rootNode,int startNodeIndex,int endNodeIndex)
{

	int curIndex = startNodeIndex;
	int walkIterations = 0;
	int subTreeSize = endNodeIndex - startNodeIndex;

	int escapeIndex;

	bool aabbOverlap, isLeafNode;

	while (curIndex < endNodeIndex)
	{
		//catch bugs in tree data
		assert (walkIterations < subTreeSize);

		walkIterations++;
		aabbOverlap = spuTestQuantizedAabbAgainstQuantizedAabb(quantizedQueryAabbMin,quantizedQueryAabbMax,rootNode->m_quantizedAabbMin,rootNode->m_quantizedAabbMax);
		isLeafNode = rootNode->isLeafNode();

		if (isLeafNode && aabbOverlap)
		{
			//printf("overlap with node %d\n",rootNode->getTriangleIndex());
			nodeCallback->processNode(0,rootNode->getTriangleIndex());
			//			spu_printf("SPU: overlap detected with triangleIndex:%d\n",rootNode->getTriangleIndex());
		} 

		if (aabbOverlap || isLeafNode)
		{
			rootNode++;
			curIndex++;
		} else
		{
			escapeIndex = rootNode->getEscapeIndex();
			rootNode += escapeIndex;
			curIndex += escapeIndex;
		}
	}

}


void small_cache_read(void* buffer, uint64_t ea, size_t size)
{
#if USE_SOFTWARE_CACHE
	// Check for alignment requirements. We need to make sure the entire request fits within one cache line,
	// so the first and last bytes should fall on the same cache line
	btAssert((ea & ~SPE_CACHELINE_MASK) == ((ea + size - 1) & ~SPE_CACHELINE_MASK));

	void* ls = spe_cache_read(ea);
	memcpy(buffer, ls, size);
#else
	stallingUnalignedDmaSmallGet(buffer,ea,size);
#endif
}



class spuNodeCallback : public btNodeOverlapCallback
{
	SpuCollisionPairInput* m_wuInput;
	SpuContactResult&		m_spuContacts;
	CollisionTask_LocalStoreMemory*	m_lsMemPtr;

	ATTRIBUTE_ALIGNED16(btVector3	spuTriangleVertices[3]);
	ATTRIBUTE_ALIGNED16(btScalar	spuUnscaledVertex[4]);
	ATTRIBUTE_ALIGNED16(int	spuIndices[16]);


public:
	spuNodeCallback(SpuCollisionPairInput* wuInput, CollisionTask_LocalStoreMemory*	lsMemPtr,SpuContactResult& spuContacts)
		:	m_wuInput(wuInput),
		m_lsMemPtr(lsMemPtr),
		m_spuContacts(spuContacts)
	{
	}

	virtual void processNode(int subPart, int triangleIndex)
	{
		///Create a triangle on the stack, call process collision, with GJK
		///DMA the vertices, can benefit from software caching

		//		spu_printf("processNode with triangleIndex %d\n",triangleIndex);



		int* indexBasePtr = (int*)(m_lsMemPtr->gIndexMesh.m_triangleIndexBase+triangleIndex*m_lsMemPtr->gIndexMesh.m_triangleIndexStride);

		///DMA the indices
		small_cache_read(&m_lsMemPtr->spuIndices[0],(uint64_t)&indexBasePtr[0],sizeof(int));
		small_cache_read(&m_lsMemPtr->spuIndices[1],(uint64_t)&indexBasePtr[1],sizeof(int));
		small_cache_read(&m_lsMemPtr->spuIndices[2],(uint64_t)&indexBasePtr[2],sizeof(int));

		//		spu_printf("SPU index0=%d ,",spuIndices[0]);
		//		spu_printf("SPU index1=%d ,",spuIndices[1]);
		//		spu_printf("SPU index2=%d ,",spuIndices[2]);
		//		spu_printf("SPU: indexBasePtr=%llx\n",indexBasePtr);

		const btVector3& meshScaling = m_lsMemPtr->gTriangleMeshInterface.getScaling();
		for (int j=2;j>=0;j--)
		{
			int graphicsindex = m_lsMemPtr->spuIndices[j];

			//			spu_printf("SPU index=%d ,",graphicsindex);
			btScalar* graphicsbasePtr = (btScalar*)(m_lsMemPtr->gIndexMesh.m_vertexBase+graphicsindex*m_lsMemPtr->gIndexMesh.m_vertexStride);
			//			spu_printf("SPU graphicsbasePtr=%llx\n",graphicsbasePtr);


			///handle un-aligned vertices...

			//another DMA for each vertex
			small_cache_read(&spuUnscaledVertex[0],(uint64_t)&graphicsbasePtr[0],sizeof(btScalar));
			small_cache_read(&spuUnscaledVertex[1],(uint64_t)&graphicsbasePtr[1],sizeof(btScalar));
			small_cache_read(&spuUnscaledVertex[2],(uint64_t)&graphicsbasePtr[2],sizeof(btScalar));

			spuTriangleVertices[j] = btVector3(
				spuUnscaledVertex[0]*meshScaling.getX(),
				spuUnscaledVertex[1]*meshScaling.getY(),
				spuUnscaledVertex[2]*meshScaling.getZ());

			//			spu_printf("SPU:triangle vertices:%f,%f,%f\n",spuTriangleVertices[j].x(),spuTriangleVertices[j].y(),spuTriangleVertices[j].z());
		}



		//btTriangleShape	tmpTriangleShape(spuTriangleVertices[0],spuTriangleVertices[1],spuTriangleVertices[2]);


		SpuCollisionPairInput triangleConcaveInput(*m_wuInput);
		triangleConcaveInput.m_spuCollisionShapes[1] = &spuTriangleVertices[0];
		triangleConcaveInput.m_shapeType1 = TRIANGLE_SHAPE_PROXYTYPE;

		m_spuContacts.setShapeIdentifiers(-1,-1,subPart,triangleIndex);

		//		m_spuContacts.flush();

		ProcessSpuConvexConvexCollision(&triangleConcaveInput, m_lsMemPtr,m_spuContacts);
		///this flush should be automatic
		//	m_spuContacts.flush();
	}

};




////////////////////////
/// Convex versus Concave triangle mesh collision detection (handles concave triangle mesh versus sphere, box, cylinder, triangle, cone, convex polyhedron etc)
///////////////////
void	ProcessConvexConcaveSpuCollision(SpuCollisionPairInput* wuInput, CollisionTask_LocalStoreMemory* lsMemPtr, SpuContactResult& spuContacts)
{
	//order: first collision shape is convex, second concave. m_isSwapped is true, if the original order was opposite





	btBvhTriangleMeshShape*	trimeshShape = (btBvhTriangleMeshShape*)wuInput->m_spuCollisionShapes[1];
	//need the mesh interface, for access to triangle vertices
	{
		int dmaSize = sizeof(btTriangleIndexVertexArray);
		uint64_t	dmaPpuAddress2 = reinterpret_cast<uint64_t>(trimeshShape->getMeshInterface());
		//	spu_printf("trimeshShape->getMeshInterface() == %llx\n",dmaPpuAddress2);
		cellDmaGet(&lsMemPtr->gTriangleMeshInterface, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
		cellDmaWaitTagStatusAll(DMA_MASK(1));
	}

	///now DMA over the BVH
	{
		int dmaSize = sizeof(btOptimizedBvh);
		uint64_t	dmaPpuAddress2 = reinterpret_cast<uint64_t>(trimeshShape->getOptimizedBvh());
		//spu_printf("trimeshShape->getOptimizedBvh() == %llx\n",dmaPpuAddress2);
		cellDmaGet(&lsMemPtr->gOptimizedBvh, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
		cellDmaWaitTagStatusAll(DMA_MASK(1));
	}

	btVector3 aabbMin(-1,-400,-1);
	btVector3 aabbMax(1,400,1);


	//recalc aabbs
	btTransform convexInTriangleSpace;
	convexInTriangleSpace = wuInput->m_worldTransform1.inverse() * wuInput->m_worldTransform0;
	btConvexInternalShape* convexShape = (btConvexInternalShape*)wuInput->m_spuCollisionShapes[0];
	//calculate the aabb, given the types...
	switch (wuInput->m_shapeType0)
	{
	case CYLINDER_SHAPE_PROXYTYPE:

	case BOX_SHAPE_PROXYTYPE:
		{
			float margin=convexShape->getMarginNV();
			btVector3 halfExtents = convexShape->getImplicitShapeDimensions();
			btTransform& t = convexInTriangleSpace;
			btMatrix3x3 abs_b = t.getBasis().absolute();  
			btPoint3 center = t.getOrigin();
			btVector3 extent = btVector3(abs_b[0].dot(halfExtents),
				abs_b[1].dot(halfExtents),
				abs_b[2].dot(halfExtents));
			extent += btVector3(margin,margin,margin);
			aabbMin = center - extent;
			aabbMax = center + extent;
			break;
		}

	case CAPSULE_SHAPE_PROXYTYPE:
		{
			float margin=convexShape->getMarginNV();
			btVector3 halfExtents = convexShape->getImplicitShapeDimensions();
			//add the radius to y-axis to get full height
			btScalar radius = halfExtents[0];
			halfExtents[1] += radius;
			btTransform& t = convexInTriangleSpace;
			btMatrix3x3 abs_b = t.getBasis().absolute();  
			btPoint3 center = t.getOrigin();
			btVector3 extent = btVector3(abs_b[0].dot(halfExtents),
				abs_b[1].dot(halfExtents),
				abs_b[2].dot(halfExtents));
			extent += btVector3(margin,margin,margin);
			aabbMin = center - extent;
			aabbMax = center + extent;
			break;
		}


	case SPHERE_SHAPE_PROXYTYPE:
		{
			float radius = convexShape->getImplicitShapeDimensions().getX();// * convexShape->getLocalScaling().getX();
			float margin = radius + convexShape->getMarginNV();
			btTransform& t = convexInTriangleSpace;
			const btVector3& center = t.getOrigin();
			btVector3 extent(margin,margin,margin);
			aabbMin = center - extent;
			aabbMax = center + extent;
			break;
		}
	case CONVEX_HULL_SHAPE_PROXYTYPE:
		{
			int dmaSize = sizeof(btConvexHullShape);
			uint64_t	dmaPpuAddress2 = wuInput->m_collisionShapes[0];

			ATTRIBUTE_ALIGNED16(char convexHullShape0[sizeof(btConvexHullShape)]);

			cellDmaGet(&convexHullShape0, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(1));
			btConvexHullShape* localPtr = (btConvexHullShape*)&convexHullShape0;
			btTransform& t = convexInTriangleSpace;

			btScalar margin = convexShape->getMarginNV();

			localPtr->getNonvirtualAabb(t,aabbMin,aabbMax,margin);

			//spu_printf("SPU convex aabbMin=%f,%f,%f=\n",aabbMin.getX(),aabbMin.getY(),aabbMin.getZ());
			//spu_printf("SPU convex aabbMax=%f,%f,%f=\n",aabbMax.getX(),aabbMax.getY(),aabbMax.getZ());

			break;
		}

	default:
		spu_printf("SPU: unsupported shapetype %d in AABB calculation\n");
	};

	//CollisionShape* triangleShape = static_cast<btCollisionShape*>(triBody->m_collisionShape);
	//convexShape->getAabb(convexInTriangleSpace,m_aabbMin,m_aabbMax);

	//	btScalar extraMargin = collisionMarginTriangle;
	//	btVector3 extra(extraMargin,extraMargin,extraMargin);
	//	aabbMax += extra;
	//	aabbMin -= extra;



	///quantize query AABB
	unsigned short int quantizedQueryAabbMin[3];
	unsigned short int quantizedQueryAabbMax[3];
	lsMemPtr->gOptimizedBvh.quantizeWithClamp(quantizedQueryAabbMin,aabbMin);
	lsMemPtr->gOptimizedBvh.quantizeWithClamp(quantizedQueryAabbMax,aabbMax);

	QuantizedNodeArray&	nodeArray = lsMemPtr->gOptimizedBvh.getQuantizedNodeArray();
	//spu_printf("SPU: numNodes = %d\n",nodeArray.size());

	BvhSubtreeInfoArray& subTrees = lsMemPtr->gOptimizedBvh.getSubtreeInfoArray();

	spuNodeCallback	nodeCallback(wuInput,lsMemPtr,spuContacts);
	IndexedMeshArray&	indexArray = lsMemPtr->gTriangleMeshInterface.getIndexedMeshArray();
	//spu_printf("SPU:indexArray.size() = %d\n",indexArray.size());


	//	spu_printf("SPU: numSubTrees = %d\n",subTrees.size());
	//not likely to happen
	if (subTrees.size() && indexArray.size() == 1)
	{
		///DMA in the index info
		{
			int dmaSize = sizeof(btIndexedMesh);
			uint64_t	dmaPpuAddress2 = reinterpret_cast<uint64_t>(&indexArray[0]);
			cellDmaGet(&lsMemPtr->gIndexMesh, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(1));
		}

		//spu_printf("SPU gIndexMesh dma finished\n");

		//display the headers
		int numBatch = subTrees.size();
		for (int i=0;i<numBatch;)
		{

			int remaining = subTrees.size() - i;
			int nextBatch = remaining < MAX_SPU_SUBTREE_HEADERS ? remaining : MAX_SPU_SUBTREE_HEADERS;
			{
				int dmaSize = nextBatch* sizeof(btBvhSubtreeInfo);
				uint64_t	dmaPpuAddress2 = reinterpret_cast<uint64_t>(&subTrees[i]);
				//				spu_printf("&subtree[i]=%llx, dmaSize = %d\n",dmaPpuAddress2,dmaSize);
				cellDmaGet(&lsMemPtr->gSubtreeHeaders[0], dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
				cellDmaWaitTagStatusAll(DMA_MASK(1));
			}

			//			spu_printf("nextBatch = %d\n",nextBatch);

			for (int j=0;j<nextBatch;j++)
			{
				const btBvhSubtreeInfo& subtree = lsMemPtr->gSubtreeHeaders[j];

				bool overlap = spuTestQuantizedAabbAgainstQuantizedAabb(quantizedQueryAabbMin,quantizedQueryAabbMax,subtree.m_quantizedAabbMin,subtree.m_quantizedAabbMax);
				if (overlap)
				{
					btAssert(subtree.m_subtreeSize);

					//dma the actual nodes of this subtree
					{
						int dmaSize = subtree.m_subtreeSize* sizeof(btQuantizedBvhNode);
						uint64_t	dmaPpuAddress2 = reinterpret_cast<uint64_t>(&nodeArray[subtree.m_rootNodeIndex]);
						cellDmaGet(&lsMemPtr->gSubtreeNodes[0], dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
						cellDmaWaitTagStatusAll(DMA_MASK(1));
					}



					spuWalkStacklessQuantizedTree(&nodeCallback,quantizedQueryAabbMin,quantizedQueryAabbMax,
						&lsMemPtr->gSubtreeNodes[0],
						0,
						subtree.m_subtreeSize);

				}


				//				spu_printf("subtreeSize = %d\n",gSubtreeHeaders[j].m_subtreeSize);
			}

			//	unsigned short int	m_quantizedAabbMin[3];
			//	unsigned short int	m_quantizedAabbMax[3];
			//	int			m_rootNodeIndex;
			//	int			m_subtreeSize;
			i+=nextBatch;
		}

		//pre-fetch first tree, then loop and double buffer



	}

}

///getShapeTypeSize could easily be optimized, but it is not likely a bottleneck
int		getShapeTypeSize(int shapeType)
{


	switch (shapeType)
	{
	case CYLINDER_SHAPE_PROXYTYPE:
		{
			int shapeSize = sizeof(btCylinderShape);
			btAssert(shapeSize < MAX_SHAPE_SIZE);
			return shapeSize;
		}
	case BOX_SHAPE_PROXYTYPE:
		{
			int shapeSize = sizeof(btBoxShape);
			btAssert(shapeSize < MAX_SHAPE_SIZE);
			return shapeSize;
		}
	case SPHERE_SHAPE_PROXYTYPE:
		{
			int shapeSize = sizeof(btSphereShape);
			btAssert(shapeSize < MAX_SHAPE_SIZE);
			return shapeSize;
		}
	case TRIANGLE_MESH_SHAPE_PROXYTYPE:
		{
			int shapeSize = sizeof(btBvhTriangleMeshShape);
			btAssert(shapeSize < MAX_SHAPE_SIZE);
			return shapeSize;
		}
	case CAPSULE_SHAPE_PROXYTYPE:
		{
			int shapeSize = sizeof(btCapsuleShape);
			btAssert(shapeSize < MAX_SHAPE_SIZE);
			return shapeSize;
		}

	case CONVEX_HULL_SHAPE_PROXYTYPE:
		{
			int shapeSize = sizeof(btConvexHullShape);
			btAssert(shapeSize < MAX_SHAPE_SIZE);
			return shapeSize;
		}

	case COMPOUND_SHAPE_PROXYTYPE:
		{
			int shapeSize = sizeof(btCompoundShape);
			btAssert(shapeSize < MAX_SHAPE_SIZE);
			return shapeSize;
		}

	default:
		btAssert(0);
		//unsupported shapetype, please add here
		break;
	}
}




////////////////////////
/// Convex versus Convex collision detection (handles collision between sphere, box, cylinder, triangle, cone, convex polyhedron etc)
///////////////////
void	ProcessSpuConvexConvexCollision(SpuCollisionPairInput* wuInput, CollisionTask_LocalStoreMemory* lsMemPtr, SpuContactResult& spuContacts)
{

#ifdef DEBUG_SPU_COLLISION_DETECTION
	//spu_printf("SPU: ProcessSpuConvexConvexCollision\n");
#endif //DEBUG_SPU_COLLISION_DETECTION
	//CollisionShape* shape0 = (CollisionShape*)wuInput->m_collisionShapes[0];
	//CollisionShape* shape1 = (CollisionShape*)wuInput->m_collisionShapes[1];
	btPersistentManifold* manifold = (btPersistentManifold*)wuInput->m_persistentManifoldPtr;



	bool genericGjk = true;



	if (genericGjk)
	{
		//try generic GJK

		SpuVoronoiSimplexSolver vsSolver;
		SpuMinkowskiPenetrationDepthSolver	penetrationSolver;



		///DMA in the vertices for convex shapes

		if (wuInput->m_shapeType0== CONVEX_HULL_SHAPE_PROXYTYPE)
		{
			//	spu_printf("SPU: DMA btConvexHullShape\n");
			ATTRIBUTE_ALIGNED16(char convexHullShape0[sizeof(btConvexHullShape)]);
			{
				int dmaSize = sizeof(btConvexHullShape);
				uint64_t	dmaPpuAddress2 = wuInput->m_collisionShapes[0];
				cellDmaGet(&convexHullShape0, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
				cellDmaWaitTagStatusAll(DMA_MASK(1));
			}
			btConvexHullShape* localPtr = (btConvexHullShape*)&convexHullShape0;

			lsMemPtr->convexVertexData.gNumConvexPoints0 = localPtr->getNumPoints();
			if (lsMemPtr->convexVertexData.gNumConvexPoints0>MAX_NUM_SPU_CONVEX_POINTS)
			{
				btAssert(0);
				spu_printf("SPU: Error: MAX_NUM_SPU_CONVEX_POINTS(%d) exceeded: %d\n",MAX_NUM_SPU_CONVEX_POINTS,lsMemPtr->convexVertexData.gNumConvexPoints0);
				return;
			}

			{
				int dmaSize = lsMemPtr->convexVertexData.gNumConvexPoints0*sizeof(btPoint3);
				uint64_t	dmaPpuAddress2 = (uint64_t) localPtr->getPoints();
				cellDmaGet(&lsMemPtr->convexVertexData.g_convexPointBuffer0, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);

				lsMemPtr->convexVertexData.gSpuConvexShapePtr0 = wuInput->m_spuCollisionShapes[0];
				lsMemPtr->convexVertexData.gConvexPoints0 = &lsMemPtr->convexVertexData.g_convexPointBuffer0[0];
				cellDmaWaitTagStatusAll(DMA_MASK(1));
			}

		}

		if (wuInput->m_shapeType1 == CONVEX_HULL_SHAPE_PROXYTYPE)
		{

			ATTRIBUTE_ALIGNED16(char convexHullShape1[sizeof(btConvexHullShape)]);

			//	spu_printf("SPU: DMA btConvexHullShape\n");
			{
				int dmaSize = sizeof(btConvexHullShape);
				uint64_t	dmaPpuAddress2 = wuInput->m_collisionShapes[1];
				cellDmaGet(&convexHullShape1, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
				cellDmaWaitTagStatusAll(DMA_MASK(1));
			}
			btConvexHullShape* localPtr = (btConvexHullShape*)&convexHullShape1;

			lsMemPtr->convexVertexData.gNumConvexPoints1 = localPtr->getNumPoints();
			if (lsMemPtr->convexVertexData.gNumConvexPoints1>MAX_NUM_SPU_CONVEX_POINTS)
			{
				btAssert(0);
				spu_printf("SPU: Error: MAX_NUM_SPU_CONVEX_POINTS(%d) exceeded: %d\n",MAX_NUM_SPU_CONVEX_POINTS,lsMemPtr->convexVertexData.gNumConvexPoints1);
				return;
			}

			{
				int dmaSize = lsMemPtr->convexVertexData.gNumConvexPoints1*sizeof(btPoint3);
				uint64_t	dmaPpuAddress2 = (uint64_t) localPtr->getPoints();
				cellDmaGet(&lsMemPtr->convexVertexData.g_convexPointBuffer1, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
				cellDmaWaitTagStatusAll(DMA_MASK(1));
				lsMemPtr->convexVertexData.gSpuConvexShapePtr1 = wuInput->m_spuCollisionShapes[1];
				lsMemPtr->convexVertexData.gConvexPoints1 = &lsMemPtr->convexVertexData.g_convexPointBuffer1[0];
			}

		}






		void* shape0Ptr = wuInput->m_spuCollisionShapes[0];
		void* shape1Ptr = wuInput->m_spuCollisionShapes[1];
		int shapeType0 = wuInput->m_shapeType0;
		int shapeType1 = wuInput->m_shapeType1;
		float marginA = wuInput->m_collisionMargin0;
		float marginB = wuInput->m_collisionMargin1;

		SpuClosestPointInput	cpInput;
		cpInput.m_convexVertexData = &lsMemPtr->convexVertexData;
		cpInput.m_transformA = wuInput->m_worldTransform0;
		cpInput.m_transformB = wuInput->m_worldTransform1;
		float sumMargin = (marginA+marginB+lsMemPtr->gPersistentManifold.getContactBreakingThreshold());
		cpInput.m_maximumDistanceSquared = sumMargin * sumMargin;

		uint64_t manifoldAddress = (uint64_t)manifold;
		btPersistentManifold* spuManifold=&lsMemPtr->gPersistentManifold;
		//spuContacts.setContactInfo(spuManifold,manifoldAddress,wuInput->m_worldTransform0,wuInput->m_worldTransform1,wuInput->m_isSwapped);
		spuContacts.setContactInfo(spuManifold,manifoldAddress,lsMemPtr->gColObj0.getWorldTransform(),lsMemPtr->gColObj1.getWorldTransform(),wuInput->m_isSwapped);

		SpuGjkPairDetector gjk(shape0Ptr,shape1Ptr,shapeType0,shapeType1,marginA,marginB,&vsSolver,&penetrationSolver);
		gjk.getClosestPoints(cpInput,spuContacts);//,debugDraw);
	}


}

template<typename T>
void DoSwap(T& a, T& b)
{
	char tmp[sizeof(T)];
	memcpy(tmp, &a, sizeof(T));
	memcpy(&a, &b, sizeof(T));
	memcpy(&b, tmp, sizeof(T));
}

void	dmaAndSetupCollisionObjects(SpuCollisionPairInput& collisionPairInput, CollisionTask_LocalStoreMemory& lsMem)
{
	{
		int dmaSize = sizeof(btCollisionObject);
		uint64_t	dmaPpuAddress2 = /*collisionPairInput.m_isSwapped ? (uint64_t)lsMem.gProxyPtr1->m_clientObject :*/ (uint64_t)lsMem.gProxyPtr0->m_clientObject;
		cellDmaGet(&lsMem.gColObj0, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);		
	}
	{
		int dmaSize = sizeof(btCollisionObject);
		uint64_t	dmaPpuAddress2 = /*collisionPairInput.m_isSwapped ? (uint64_t)lsMem.gProxyPtr0->m_clientObject :*/ (uint64_t)lsMem.gProxyPtr1->m_clientObject;
		cellDmaGet(&lsMem.gColObj1, dmaPpuAddress2  , dmaSize, DMA_TAG(2), 0, 0);		
	}

	cellDmaWaitTagStatusAll(DMA_MASK(1) | DMA_MASK(2));

	collisionPairInput.m_worldTransform0 = lsMem.gColObj0.getWorldTransform();
	collisionPairInput.m_worldTransform1 = lsMem.gColObj1.getWorldTransform();



#ifdef DEBUG_SPU_COLLISION_DETECTION
	spu_printf("SPU worldTrans0.origin = (%f,%f,%f)\n",
		collisionPairInput->m_worldTransform0.getOrigin().getX(),
		collisionPairInput->m_worldTransform0.getOrigin().getY(),
		collisionPairInput->m_worldTransform0.getOrigin().getZ());

	spu_printf("SPU worldTrans1.origin = (%f,%f,%f)\n",
		collisionPairInput->m_worldTransform1.getOrigin().getX(),
		collisionPairInput->m_worldTransform1.getOrigin().getY(),
		collisionPairInput->m_worldTransform1.getOrigin().getZ());
#endif //DEBUG_SPU_COLLISION_DETECTION

}

void	handleCollisionPair(SpuCollisionPairInput& collisionPairInput, CollisionTask_LocalStoreMemory& lsMem,
							SpuContactResult &spuContacts,
							uint64_t collisionShape0Ptr, void* collisionShape0Loc,
							uint64_t collisionShape1Ptr, void* collisionShape1Loc, bool dmaShapes = true)
{
	if (btBroadphaseProxy::isConvex(collisionPairInput.m_shapeType0) 
		&& btBroadphaseProxy::isConvex(collisionPairInput.m_shapeType1))
	{

		//dmaAndSetupCollisionObjects(collisionPairInput, lsMem);

		if (dmaShapes)
		{
			{
				int dmaSize = getShapeTypeSize(collisionPairInput.m_shapeType0);
				//uint64_t	dmaPpuAddress2 = (uint64_t)lsMem.gColObj0.getCollisionShape();
				uint64_t	dmaPpuAddress2 = collisionShape0Ptr;
				cellDmaGet(collisionShape0Loc, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
				cellDmaWaitTagStatusAll(DMA_MASK(1));
			}
			{
				int dmaSize = getShapeTypeSize(collisionPairInput.m_shapeType1);
				uint64_t	dmaPpuAddress2 = collisionShape1Ptr;
				cellDmaGet(collisionShape1Loc, dmaPpuAddress2  , dmaSize, DMA_TAG(2), 0, 0);
				cellDmaWaitTagStatusAll(DMA_MASK(2));
			}
		}

		btConvexInternalShape* spuConvexShape0 = (btConvexInternalShape*)collisionShape0Loc;
		btConvexInternalShape* spuConvexShape1 = (btConvexInternalShape*)collisionShape1Loc;

		btVector3 dim0 = spuConvexShape0->getImplicitShapeDimensions();
		btVector3 dim1 = spuConvexShape1->getImplicitShapeDimensions();

		collisionPairInput.m_primitiveDimensions0 = dim0;
		collisionPairInput.m_primitiveDimensions1 = dim1;
		collisionPairInput.m_collisionShapes[0] = collisionShape0Ptr;
		collisionPairInput.m_collisionShapes[1] = collisionShape1Ptr;
		collisionPairInput.m_spuCollisionShapes[0] = spuConvexShape0;
		collisionPairInput.m_spuCollisionShapes[1] = spuConvexShape1;
		ProcessSpuConvexConvexCollision(&collisionPairInput,&lsMem,spuContacts);
	} 
	else if (btBroadphaseProxy::isCompound(collisionPairInput.m_shapeType0) && 
			btBroadphaseProxy::isCompound(collisionPairInput.m_shapeType1))
	{
		//snPause();

		// Both are compounds, do N^2 CD for now
		// TODO: add some AABB-based pruning
		{
			int dmaSize = getShapeTypeSize(collisionPairInput.m_shapeType0);
			uint64_t	dmaPpuAddress2 = collisionShape0Ptr;
			cellDmaGet(collisionShape0Loc, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(1));
		}
		{
			int dmaSize = getShapeTypeSize(collisionPairInput.m_shapeType1);
			uint64_t	dmaPpuAddress2 = collisionShape1Ptr;
			cellDmaGet(collisionShape1Loc, dmaPpuAddress2  , dmaSize, DMA_TAG(2), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(2));
		}

		btCompoundShape* spuCompoundShape0 = (btCompoundShape*)collisionShape0Loc;
		btCompoundShape* spuCompoundShape1 = (btCompoundShape*)collisionShape1Loc;

		int childShapeCount0 = spuCompoundShape0->getNumChildShapes();
		int childShapeCount1 = spuCompoundShape1->getNumChildShapes();

		// dma the first list of child shapes
		{
			int dmaSize = childShapeCount0 * sizeof(btCompoundShapeChild);
			uint64_t	dmaPpuAddress2 = (uint64_t)spuCompoundShape0->getChildList();
			cellDmaGet(lsMem.gSubshapes, dmaPpuAddress2, dmaSize, DMA_TAG(1), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(1));
		}

		// dma the second list of child shapes
		{
			int dmaSize = childShapeCount1 * sizeof(btCompoundShapeChild);
			uint64_t	dmaPpuAddress2 = (uint64_t)spuCompoundShape1->getChildList();
			cellDmaGet(&lsMem.gSubshapes[MAX_SPU_COMPOUND_SUBSHAPES], dmaPpuAddress2, dmaSize, DMA_TAG(2), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(2));
		}

		// DMA all the subshapes 
		for (int i = 0; i < childShapeCount0; ++i)
		{
			btCompoundShapeChild& childShape = lsMem.gSubshapes[i];

			int dmaSize = getShapeTypeSize(childShape.m_childShapeType);
			uint64_t	dmaPpuAddress2 = (uint64_t)childShape.m_childShape;
			cellDmaGet(lsMem.gSubshapeShape[i], dmaPpuAddress2, dmaSize, DMA_TAG(1), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(1));
		}

		for (int i = 0; i < childShapeCount1; ++i)
		{
			btCompoundShapeChild& childShape = lsMem.gSubshapes[MAX_SPU_COMPOUND_SUBSHAPES+i];

			int dmaSize = getShapeTypeSize(childShape.m_childShapeType);
			uint64_t	dmaPpuAddress2 = (uint64_t)childShape.m_childShape;
			cellDmaGet(lsMem.gSubshapeShape[MAX_SPU_COMPOUND_SUBSHAPES+i], dmaPpuAddress2, dmaSize, DMA_TAG(1), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(1));
		}

		// Start the N^2
		for (int i = 0; i < childShapeCount0; ++i)
		{
			btCompoundShapeChild& childShape0 = lsMem.gSubshapes[i];

			for (int j = 0; j < childShapeCount1; ++j)
			{
				btCompoundShapeChild& childShape1 = lsMem.gSubshapes[MAX_SPU_COMPOUND_SUBSHAPES+j];

				SpuCollisionPairInput cinput (collisionPairInput);
				cinput.m_worldTransform0 = collisionPairInput.m_worldTransform0 * childShape0.m_transform;
				cinput.m_shapeType0 = childShape0.m_childShapeType;
				cinput.m_collisionMargin0 = childShape0.m_childMargin;

				cinput.m_worldTransform1 = collisionPairInput.m_worldTransform1 * childShape1.m_transform;
				cinput.m_shapeType1 = childShape1.m_childShapeType;
				cinput.m_collisionMargin1 = childShape1.m_childMargin;

				handleCollisionPair(cinput, lsMem, spuContacts,			
					(uint64_t)childShape0.m_childShape, lsMem.gSubshapeShape[i], 
					(uint64_t)childShape1.m_childShape, lsMem.gSubshapeShape[MAX_SPU_COMPOUND_SUBSHAPES+i], false);
			}
		}
	}
	else if (btBroadphaseProxy::isCompound(collisionPairInput.m_shapeType0) )
	{
		//snPause();
		{
			int dmaSize = getShapeTypeSize(collisionPairInput.m_shapeType0);
			uint64_t	dmaPpuAddress2 = collisionShape0Ptr;
			cellDmaGet(collisionShape0Loc, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(1));
		}
		{
			int dmaSize = getShapeTypeSize(collisionPairInput.m_shapeType1);
			uint64_t	dmaPpuAddress2 = collisionShape1Ptr;
			cellDmaGet(collisionShape1Loc, dmaPpuAddress2  , dmaSize, DMA_TAG(2), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(2));
		}

		// object 0 compound, object 1 non-compound
		btCompoundShape* spuCompoundShape = (btCompoundShape*)collisionShape0Loc;

		int childShapeCount = spuCompoundShape->getNumChildShapes();

		// dma the list of child shapes
		{
			int dmaSize = childShapeCount * sizeof(btCompoundShapeChild);
			uint64_t	dmaPpuAddress2 = (uint64_t)spuCompoundShape->getChildList();
			cellDmaGet(lsMem.gSubshapes, dmaPpuAddress2, dmaSize, DMA_TAG(1), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(1));
		}

		for (int i = 0; i < childShapeCount; ++i)
		{
			btCompoundShapeChild& childShape = lsMem.gSubshapes[i];

			// Dma the child shape
			{
				int dmaSize = getShapeTypeSize(childShape.m_childShapeType);
				uint64_t	dmaPpuAddress2 = (uint64_t)childShape.m_childShape;
				cellDmaGet(lsMem.gSubshapeShape[i], dmaPpuAddress2, dmaSize, DMA_TAG(1), 0, 0);
				cellDmaWaitTagStatusAll(DMA_MASK(1));
			}

			SpuCollisionPairInput cinput (collisionPairInput);
			cinput.m_worldTransform0 = collisionPairInput.m_worldTransform0 * childShape.m_transform;
			cinput.m_shapeType0 = childShape.m_childShapeType;
			cinput.m_collisionMargin0 = childShape.m_childMargin;

			handleCollisionPair(cinput, lsMem, spuContacts,			
				(uint64_t)childShape.m_childShape, lsMem.gSubshapeShape[i], 
				collisionShape1Ptr, collisionShape1Loc, false);
		}
	}
	else if (btBroadphaseProxy::isCompound(collisionPairInput.m_shapeType1) )
	{
		//snPause();
		{
			int dmaSize = getShapeTypeSize(collisionPairInput.m_shapeType0);
			uint64_t	dmaPpuAddress2 = collisionShape0Ptr;
			cellDmaGet(collisionShape0Loc, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(1));
		}
		{
			int dmaSize = getShapeTypeSize(collisionPairInput.m_shapeType1);
			uint64_t	dmaPpuAddress2 = collisionShape1Ptr;
			cellDmaGet(collisionShape1Loc, dmaPpuAddress2  , dmaSize, DMA_TAG(2), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(2));
		}

		// object 0 non-compound, object 1 compound
		btCompoundShape* spuCompoundShape = (btCompoundShape*)collisionShape1Loc;

		int childShapeCount = spuCompoundShape->getNumChildShapes();

		// dma the list of child shapes
		{
			int dmaSize = childShapeCount * sizeof(btCompoundShapeChild);
			uint64_t	dmaPpuAddress2 = (uint64_t)spuCompoundShape->getChildList();
			cellDmaGet(lsMem.gSubshapes, dmaPpuAddress2, dmaSize, DMA_TAG(1), 0, 0);
			cellDmaWaitTagStatusAll(DMA_MASK(1));
		}

		for (int i = 0; i < childShapeCount; ++i)
		{
			btCompoundShapeChild& childShape = lsMem.gSubshapes[i];

			// Dma the child shape
			{
				int dmaSize = getShapeTypeSize(childShape.m_childShapeType);
				uint64_t	dmaPpuAddress2 = (uint64_t)childShape.m_childShape;
				cellDmaGet(lsMem.gSubshapeShape[i], dmaPpuAddress2, dmaSize, DMA_TAG(1), 0, 0);
				cellDmaWaitTagStatusAll(DMA_MASK(1));
			}

			SpuCollisionPairInput cinput (collisionPairInput);
			cinput.m_worldTransform1 = collisionPairInput.m_worldTransform1 * childShape.m_transform;
			cinput.m_shapeType1 = childShape.m_childShapeType;
			cinput.m_collisionMargin1 = childShape.m_childMargin;

			handleCollisionPair(cinput, lsMem, spuContacts,
				collisionShape0Ptr, collisionShape0Loc, 
				(uint64_t)childShape.m_childShape, lsMem.gSubshapeShape[i], false);
		}
		
	}
	else
	{
		//a non-convex shape is involved									
		bool handleConvexConcave = false;

		//snPause();

		if (btBroadphaseProxy::isConcave(collisionPairInput.m_shapeType0) &&
			btBroadphaseProxy::isConvex(collisionPairInput.m_shapeType1))
		{
			// Swap stuff
			DoSwap(collisionShape0Ptr, collisionShape1Ptr);
			DoSwap(collisionShape0Loc, collisionShape1Loc);
			DoSwap(collisionPairInput.m_shapeType0, collisionPairInput.m_shapeType1);
			DoSwap(collisionPairInput.m_worldTransform0, collisionPairInput.m_worldTransform1);
			DoSwap(collisionPairInput.m_collisionMargin0, collisionPairInput.m_collisionMargin1);
			
			collisionPairInput.m_isSwapped = true;
		}
		
		if (btBroadphaseProxy::isConvex(collisionPairInput.m_shapeType0)&&
			btBroadphaseProxy::isConcave(collisionPairInput.m_shapeType1))
		{
			handleConvexConcave = true;
		}
		if (handleConvexConcave)
		{

			if (dmaShapes)
			{
				///dma and initialize the convex object
				{
					int dmaSize = getShapeTypeSize(collisionPairInput.m_shapeType0);
					//uint64_t	dmaPpuAddress2 = (uint64_t)lsMem.gColObj0.getCollisionShape();
					uint64_t	dmaPpuAddress2 = collisionShape0Ptr;
					cellDmaGet(collisionShape0Loc, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
					cellDmaWaitTagStatusAll(DMA_MASK(1));
				}
				///dma and initialize the concave object
				{
					int dmaSize = getShapeTypeSize(collisionPairInput.m_shapeType1);
					uint64_t	dmaPpuAddress2 = collisionShape1Ptr;
					cellDmaGet(collisionShape1Loc, dmaPpuAddress2  , dmaSize, DMA_TAG(2), 0, 0);
					cellDmaWaitTagStatusAll(DMA_MASK(2));
				}
			}
			
			btConvexInternalShape* spuConvexShape0 = (btConvexInternalShape*)collisionShape0Loc;
			btBvhTriangleMeshShape* trimeshShape = (btBvhTriangleMeshShape*)collisionShape1Loc;

			btVector3 dim0 = spuConvexShape0->getImplicitShapeDimensions();
			collisionPairInput.m_primitiveDimensions0 = dim0;
			collisionPairInput.m_collisionShapes[0] = collisionShape0Ptr;
			collisionPairInput.m_collisionShapes[1] = collisionShape1Ptr;
			collisionPairInput.m_spuCollisionShapes[0] = spuConvexShape0;
			collisionPairInput.m_spuCollisionShapes[1] = trimeshShape;

			ProcessConvexConcaveSpuCollision(&collisionPairInput,&lsMem,spuContacts);
		}

	}

	spuContacts.flush();
}

void	processCollisionTask(void* userPtr, void* lsMemPtr)
{

	SpuGatherAndProcessPairsTaskDesc* taskDescPtr = (SpuGatherAndProcessPairsTaskDesc*)userPtr;
	SpuGatherAndProcessPairsTaskDesc& taskDesc = *taskDescPtr;
	CollisionTask_LocalStoreMemory*	colMemPtr = (CollisionTask_LocalStoreMemory*)lsMemPtr;
	CollisionTask_LocalStoreMemory& lsMem = *(colMemPtr);

	//	spu_printf("taskDescPtr=%llx\n",taskDescPtr);

	SpuContactResult spuContacts;

	////////////////////

	uint64_t dmaInPtr = taskDesc.inPtr;
	unsigned int numPages = taskDesc.numPages;
	unsigned int numOnLastPage = taskDesc.numOnLastPage;

	// prefetch first set of inputs and wait
	lsMem.g_workUnitTaskBuffers.init();

	unsigned int nextNumOnPage = (numPages > 1)? MIDPHASE_NUM_WORKUNITS_PER_PAGE : numOnLastPage;
	lsMem.g_workUnitTaskBuffers.backBufferDmaGet(dmaInPtr, nextNumOnPage*sizeof(SpuGatherAndProcessWorkUnitInput), DMA_TAG(3));
	dmaInPtr += MIDPHASE_WORKUNIT_PAGE_SIZE;

	for (unsigned int i = 0; i < numPages; i++)
	{
		// wait for back buffer dma and swap buffers
		unsigned char *inputPtr = lsMem.g_workUnitTaskBuffers.swapBuffers();

		// number on current page is number prefetched last iteration
		unsigned int numOnPage = nextNumOnPage;

		unsigned int j;


		// prefetch next set of inputs
		if (i < numPages-1)
		{
			nextNumOnPage = (i == numPages-2)? numOnLastPage : MIDPHASE_NUM_WORKUNITS_PER_PAGE;
			lsMem.g_workUnitTaskBuffers.backBufferDmaGet(dmaInPtr, nextNumOnPage*sizeof(SpuGatherAndProcessWorkUnitInput), DMA_TAG(3));
			dmaInPtr += MIDPHASE_WORKUNIT_PAGE_SIZE;
		}

		SpuGatherAndProcessWorkUnitInput* wuInputs = reinterpret_cast<SpuGatherAndProcessWorkUnitInput *>(inputPtr);

		for (j = 0; j < numOnPage; j++)
		{
#ifdef DEBUG_SPU_COLLISION_DETECTION
			printMidphaseInput(&wuInputs[j]);
#endif //DEBUG_SPU_COLLISION_DETECTION


			int numPairs = wuInputs[j].m_endIndex - wuInputs[j].m_startIndex;
			if (numPairs)
			{

				{
					int dmaSize = numPairs*sizeof(btBroadphasePair);
					uint64_t	dmaPpuAddress = wuInputs[j].m_pairArrayPtr+wuInputs[j].m_startIndex * sizeof(btBroadphasePair);
					cellDmaGet(&lsMem.gBroadphasePairs, dmaPpuAddress  , dmaSize, DMA_TAG(1), 0, 0);
					cellDmaWaitTagStatusAll(DMA_MASK(1));
				}

				for (int p=0;p<numPairs;p++)
				{
					//for each broadphase pair, do something

					btBroadphasePair& pair = lsMem.gBroadphasePairs[p];
#ifdef DEBUG_SPU_COLLISION_DETECTION
					spu_printf("pair->m_userInfo = %d\n",pair.m_userInfo);
					spu_printf("pair->m_algorithm = %d\n",pair.m_algorithm);
					spu_printf("pair->m_pProxy0 = %d\n",pair.m_pProxy0);
					spu_printf("pair->m_pProxy1 = %d\n",pair.m_pProxy1);
#endif //DEBUG_SPU_COLLISION_DETECTION

					int userInfo = int(pair.m_userInfo);

					if (userInfo == 2 && pair.m_algorithm && pair.m_pProxy0 && pair.m_pProxy1)
					{


						{
							int dmaSize = sizeof(SpuContactManifoldCollisionAlgorithm);
							uint64_t	dmaPpuAddress2 = (uint64_t)pair.m_algorithm;
							cellDmaGet(&lsMem.gSpuContactManifoldAlgo, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
							cellDmaWaitTagStatusAll(DMA_MASK(1));
						}



						SpuCollisionPairInput collisionPairInput;
						collisionPairInput.m_persistentManifoldPtr = (uint64_t) lsMem.gSpuContactManifoldAlgo.getContactManifoldPtr();
						collisionPairInput.m_isSwapped = false;
						//snPause();

#ifdef DEBUG_SPU_COLLISION_DETECTION
						spu_printf("SPU: manifoldPtr: %llx",collisionPairInput->m_persistentManifoldPtr);
#endif //DEBUG_SPU_COLLISION_DETECTION

						{
							int dmaSize = sizeof(btBroadphaseProxy);
							//spu_printf("dmaSize btBroadphaseProxy1 = %d\n",dmaSize);
							uint64_t	dmaPpuAddress2 = (uint64_t)pair.m_pProxy0;
							lsMem.gProxyPtr0 = (btBroadphaseProxy*) lsMem.bufferProxy0;
							//spu_printf("dmaPpuAddress2 btBroadphaseProxy1 = %llx, gProxyPtr0 = %d\n",dmaPpuAddress2,gProxyPtr0);
							stallingUnalignedDmaSmallGet(lsMem.gProxyPtr0, dmaPpuAddress2  , dmaSize);

						}
						{
							int dmaSize = sizeof(btBroadphaseProxy);
							uint64_t	dmaPpuAddress2 = (uint64_t)pair.m_pProxy1;
							lsMem.gProxyPtr1 = (btBroadphaseProxy*) lsMem.bufferProxy1;
							stallingUnalignedDmaSmallGet(lsMem.gProxyPtr1, dmaPpuAddress2  , dmaSize);
						}

						//btCollisionObject* colObj0 = (btCollisionObject*)gProxy0.m_clientObject;
						//btCollisionObject* colObj1 = (btCollisionObject*)gProxy1.m_clientObject;
						

						if (1)
						{

							///can wait on the combined DMA_MASK, or dma on the same tag

							collisionPairInput.m_shapeType0 = lsMem.gSpuContactManifoldAlgo.getShapeType0();
							collisionPairInput.m_shapeType1 = lsMem.gSpuContactManifoldAlgo.getShapeType1();
							collisionPairInput.m_collisionMargin0 = lsMem.gSpuContactManifoldAlgo.getCollisionMargin0();
							collisionPairInput.m_collisionMargin1 = lsMem.gSpuContactManifoldAlgo.getCollisionMargin1();

#ifdef DEBUG_SPU_COLLISION_DETECTION
							spu_printf("SPU collisionPairInput->m_shapeType0 = %d\n",collisionPairInput->m_shapeType0);
							spu_printf("SPU collisionPairInput->m_shapeType1 = %d\n",collisionPairInput->m_shapeType1);
#endif //DEBUG_SPU_COLLISION_DETECTION

							{
								int dmaSize = sizeof(btPersistentManifold);

								uint64_t	dmaPpuAddress2 = collisionPairInput.m_persistentManifoldPtr;
								cellDmaGet(&lsMem.gPersistentManifold, dmaPpuAddress2  , dmaSize, DMA_TAG(1), 0, 0);
								cellDmaWaitTagStatusAll(DMA_MASK(1));
							}

							if (1)
							{
								//snPause();

								// Get the collision objects
								dmaAndSetupCollisionObjects(collisionPairInput, lsMem);

								handleCollisionPair(collisionPairInput, lsMem, spuContacts, 
									(uint64_t)lsMem.gColObj0.getCollisionShape(), lsMem.gCollisionShape0,
									(uint64_t)lsMem.gColObj1.getCollisionShape(), lsMem.gCollisionShape1);
							}		
						}

					}
				}
			}
		} //end for (j = 0; j < numOnPage; j++)

	}//	for 


	return;
}
